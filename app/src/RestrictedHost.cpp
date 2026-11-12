#include "RestrictedHost.h"

#include "PluginHost.h"
#include "PluginHostIpc.h"

#include <atomic>
#include <thread>

#include <windows.h>
#include <appmodel.h>
#include <aclapi.h>
#include <UserEnv.h>
#include <tlhelp32.h>

// The restricted host aims for reduced rights (AppContainer or a low-
// integrity token). JUCE's ChildProcess has no surface for launching into
// an AppContainer, so the host currently runs with the user's normal token
// while the AppContainer profile is created for future use. The security
// benefit right now is process isolation (a crashing/hanging plugin does
// not take the app down); full sandboxing is a later step.

namespace {

constexpr const char* kHostFlag = "--plugin-host";

// Best effort: create the AppContainer profile once. "Already exists" is a
// success: the profile is a per-user identity, not a per-run resource.
bool ensureAppContainer() {
    // CreateAppContainerProfile lives in UserEnv.h. It creates (or reuses)
    // the named AppContainer identity for this user; launching the child
    // into that container needs EXEC_METADATA on this exe's folder, which
    // is granted separately via CreateApplicationGroup and is out of scope
    // for a per-user profile. The 4th arg `requestedCapabilities` is the
    // capability SID list; empty means no network/file capabilities, which
    // is exactly the sandbox we want.
    const HRESULT hr = CreateAppContainerProfile(
        L"PixelAxolotl.PluginRack.PluginHost",
        nullptr, nullptr, nullptr, 0, nullptr);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}

DWORD parentProcessId() {
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = ::GetCurrentProcessId();
    DWORD parent = 0;
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == self) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return parent;
}

// The app exits with TerminateProcess (plugin teardown can hang forever), so
// it cannot send a quit message. Watch its process handle instead and end the
// host when it goes away; otherwise orphaned hosts keep the exe locked and
// spin on the audio block for the rest of the session.
void terminateWhenParentExits() {
    const DWORD parentPid = parentProcessId();
    if (parentPid == 0) return;

    const HANDLE parent = ::OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (parent == nullptr) return;

    std::thread([parent] {
        ::WaitForSingleObject(parent, INFINITE);
        ::TerminateProcess(::GetCurrentProcess(), 0);
    }).detach();
}

} // namespace

RestrictedHost::RestrictedHost() = default;
RestrictedHost::~RestrictedHost() {
    killAndReap();
}

bool RestrictedHost::launch(const juce::File& file, int uniqueId,
                            const juce::String& stateBase64, juce::String& error) {
    // The profile is prepared for the day the host can be launched into it.
    // Today it does not change the child token, so say what actually runs.
    (void) ensureAppContainer();
    static std::atomic<bool> tokenNoticeLogged{false};
    if (!tokenNoticeLogged.exchange(true, std::memory_order_relaxed)) {
        juce::Logger::writeToLog("plugin: hosts run with the normal user token "
                                 "(process isolation only; AppContainer launch is not wired up)");
    }

    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    if (!executable.existsAsFile()) {
        error = "The plugin host process is missing. No plugin was loaded.";
        return false;
    }

    // Shared audio block: created in the app and handed to the host by name.
    // The section object must stay referenced by an open handle for as long
    // as the host needs to open it; a mapped view alone does not keep the
    // name resolvable (see NamedSection.h), and closing the create handle
    // here made every host exit with "audio channel unavailable".
    //
    // The name is per hosted instance, not per plugin file. A scene can use
    // the same VST3 in several slots (and the smoke probe loads one more
    // while the scene host runs), so a name keyed only by path would make
    // two hosts read and write one audio block.
    static std::atomic<unsigned> channelCounter{0};
    sharedName = pluginHostIpc::sharedNameFor(file.getFullPathName()) + "-"
                 + juce::String(static_cast<int>(++channelCounter));
    const auto size = sizeof(pluginHostIpc::SharedAudioBlock);
    if (!audioSection.create(sharedName.toWideCharPointer(), size)) {
        juce::Logger::writeToLog("plugin: audio channel '" + sharedName
                                 + "' unavailable (err "
                                 + juce::String(static_cast<int>(audioSection.error()))
                                 + ")");
        error = "Could not create the plugin audio channel. No plugin was loaded.";
        return false;
    }
    juce::Logger::writeToLog("plugin: audio channel '" + sharedName + "' ready ("
                             + juce::String(static_cast<int>(size)) + " bytes)");
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(audioSection.data());
    block->sequence = 0;
    block->responseSequence = 0;
    block->writeSequence = 0;
    block->readSequence = 0;
    block->ackSequence = 0;
    block->blockSequence = 0;
    block->lastSequence = 0;
    block->numFrames = 0;
    block->bufferSize = 0;
    block->sampleRate = 0.0;

    // Wake-up channel: the host waits on `request`, the app waits on
    // `response`. Created after the section so a failed launch cannot leave a
    // half-wired host that the app then tries to talk to.
    const auto requestName = pluginHostIpc::requestEventNameFor(sharedName);
    const auto responseName = pluginHostIpc::responseEventNameFor(sharedName);
    if (!requestEvent.create(requestName.toWideCharPointer())
        || !responseEvent.create(responseName.toWideCharPointer())) {
        juce::Logger::writeToLog(
            "plugin: audio events '" + requestName + "', '" + responseName
            + "' unavailable (err "
            + juce::String(static_cast<int>(requestEvent.error())) + "/"
            + juce::String(static_cast<int>(responseEvent.error())) + ")");
        audioSection.close();
        requestEvent.close();
        responseEvent.close();
        error = "Could not create the plugin audio events. No plugin was loaded.";
        return false;
    }

    pluginPath = file.getFullPathName();
    pluginUniqueId = uniqueId;
    configured = true;
    // The mapping name comes before the state, and an empty argument would be
    // dropped by the command-line split, shifting every later argument by one.
    // A "-" placeholder keeps the positions fixed.
    const juce::StringArray arguments { executable.getFullPathName(), kHostFlag,
                                        file.getFullPathName(), juce::String(uniqueId),
                                        sharedName,
                                        stateBase64.isEmpty() ? juce::String("-")
                                                              : stateBase64 };
    child = std::make_unique<juce::ChildProcess>();
    // NOTE: JUCE ChildProcess has no AppContainer launch surface, and
    // CreateAppContainerProfile alone does not change the child token, so
    // the host currently runs with the user's normal token. The profile is
    // still created so a future launch can use it.
    if (!child->start(arguments, juce::ChildProcess::wantStdOut)) {
        error = "The restricted plugin host could not start. No plugin was loaded.";
        child.reset();
        return false;
    }

    launched = true;
    hostDead.store(false, std::memory_order_relaxed);
    stopWatcher.store(false, std::memory_order_relaxed);
    startWatcher();
    juce::Logger::writeToLog("plugin: restricted host started for " + file.getFileName());
    return true;
}

bool RestrictedHost::configureHost(int uniqueId, const juce::String& stateBase64,
                                   juce::String& error) {
    pluginUniqueId = uniqueId;
    (void) stateBase64;
    (void) error;
    // The child was started with the UniqueId and state already in the args
    // (see launch), so this records the choice locally for callers that
    // need it after the fact.
    configured = true;
    return true;
}

bool RestrictedHost::isAlive() const {
    return launched && child != nullptr && !hostDead.load(std::memory_order_acquire);
}

bool RestrictedHost::isDead() const {
    return hostDead.load(std::memory_order_acquire);
}

void RestrictedHost::startWatcher() {
    // The watcher owns no state except the flags; child is only reset after
    // stopWatcherThread() has joined it (see killAndReap).
    watcher = std::thread([this] {
        while (!stopWatcher.load(std::memory_order_relaxed)) {
            if (child == nullptr || !child->isRunning()) {
                hostDead.store(true, std::memory_order_release);
                return;
            }
            juce::Thread::sleep(100);
        }
    });
}

void RestrictedHost::stopWatcherThread() {
    stopWatcher.store(true, std::memory_order_relaxed);
    if (watcher.joinable()) watcher.join();
}

int RestrictedHost::exitCode() const {
    return child != nullptr ? static_cast<int>(child->getExitCode()) : -1;
}

juce::String RestrictedHost::pendingOutput() {
    if (child == nullptr) return {};
    juce::String text;
    char buffer[512];
    for (;;) {
        const int read = child->readProcessOutput(buffer, static_cast<int>(sizeof(buffer)) - 1);
        if (read <= 0) break;
        buffer[read] = 0;
        text += buffer;
    }
    return text.trim();
}

juce::String RestrictedHost::sendControl(const juce::String& jsonLine, int timeoutMs) {
    (void) jsonLine;
    (void) timeoutMs;
    // Control channel lives on the child's stdout inbox; the line-based
    // protocol is implemented by runHost(). A full duplex pipe needs
    // stdin support that JUCE's ChildProcess does not offer, so the real
    // command/response round trip is deferred to the audio IPC rollout.
    return {};
}

bool RestrictedHost::submitAudioBlock(const juce::AudioBuffer<float>& input, double sampleRate,
                                      int bufferSize) {
    if (audioSection.data() == nullptr || !isAlive()) {
        return false;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(audioSection.data());
    // If the host has not acked the previous block yet, give up immediately:
    // the audio thread must never block behind the IPC. fetchAudioBlock waits
    // for that ack on the response event, so this only trips when the host is
    // genuinely late, and the caller must not overwrite the live block.
    const auto previous = block->sequence;
    if (block->responseSequence != previous) {
        return false; // host lagging; caller must not submit
    }
    const int frames = juce::jmin(input.getNumSamples(), pluginHostIpc::kMaxBlockFrames);
    // Torn-read guard: even = stable, odd = writer active. The app marks the
    // block odd, writes the header and payload, then marks it even and only
    // then publishes the sequence, so the host can tell a committed block
    // from a half-written one.
    block->blockSequence = block->blockSequence + 1; // odd: writing
    block->numFrames = frames;
    block->bufferSize = bufferSize;
    block->sampleRate = sampleRate;
    const auto* left = input.getReadPointer(0);
    const auto* right = input.getNumChannels() > 1 ? input.getReadPointer(1) : left;
    for (int i = 0; i < frames; ++i) {
        block->interleave[i * 2] = left[i];
        block->interleave[i * 2 + 1] = right[i];
    }
    block->blockSequence = block->blockSequence + 1; // even: committed
    // Payload version for the host to compare against its last read.
    block->writeSequence = block->writeSequence + 1;
    // Publish: a new sequence value means "process this block".
    const auto next = block->sequence + 1;
    block->sequence = next;
    // Wake the host. It waits on the event instead of polling, so the
    // hand-off is not bound to the system timer tick (15.6 ms in the
    // background on Windows 11) while the audio block is only a few ms long.
    requestEvent.signal();
    return true;
}

bool RestrictedHost::fetchAudioBlock(juce::AudioBuffer<float>& output,
                                     std::uint64_t expectedSequence, double& outRate,
                                     int& outBufferSize) {
    if (audioSection.data() == nullptr) {
        return false;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(audioSection.data());
    // Wait for the host to process this block on the response event. A signal
    // wakes the audio thread immediately, independent of timer resolution.
    // The counter is re-checked because a stale response (from a block whose
    // own budget already expired) can wake us before this block is answered.
    if (!responseEvent.isOpen()) return false;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    while (block->responseSequence != expectedSequence) {
        const double remaining = 20.0 - (juce::Time::getMillisecondCounterHiRes() - start);
        if (remaining <= 0.0) return false;
        if (!responseEvent.wait(static_cast<unsigned long>(juce::jmax(1.0, remaining)))) {
            if (block->responseSequence == expectedSequence) break;
            return false;
        }
    }
    // Torn-read guard: two identical blockSequence reads around the copy
    // mean the payload was stable. A mismatch means the host raced us;
    // fall through to the in-process render rather than accept garbage.
    const auto before = block->blockSequence;
    const int frames = juce::jmin(block->numFrames, pluginHostIpc::kMaxBlockFrames);
    if (output.getNumChannels() < 2 || output.getNumSamples() < frames) {
        output.setSize(2, juce::jmax(frames, 1), false, false, true);
    }
    auto* left = output.getWritePointer(0);
    auto* right = output.getWritePointer(1);
    for (int i = 0; i < frames; ++i) {
        left[i] = block->interleave[i * 2];
        right[i] = block->interleave[i * 2 + 1];
    }
    const auto after = block->blockSequence;
    if (before != after || (before % 2) == 1) {
        return false;
    }
    outRate = block->sampleRate;
    outBufferSize = block->bufferSize;
    return true;
}

void RestrictedHost::killAndReap() {
    stopWatcherThread();
    if (child != nullptr && child->isRunning()) {
        child->kill();
        child->waitForProcessToFinish(5000);
    }
    child.reset();
    audioSection.close();
    requestEvent.close();
    responseEvent.close();
    launched = false;
}

int RestrictedHost::runHost(const juce::File& file, int pluginUniqueId,
                            const juce::String& stateBase64,
                            const juce::String& sharedMappingName) {
    terminateWhenParentExits();

    // The app hands over the mapping name; only fall back to recomputing it
    // when an older caller did not pass one.
    const auto mappingName = sharedMappingName.isNotEmpty()
                                 ? sharedMappingName
                                 : pluginHostIpc::sharedNameFor(file.getFullPathName());

    // Open the channel before scanning: if the app did not keep the section
    // open, the host must say so instead of spending seconds loading a plugin
    // it can never process audio for.
    pluginHostIpc::NamedSection audioSection;
    if (!audioSection.open(mappingName.toWideCharPointer(),
                           sizeof(pluginHostIpc::SharedAudioBlock))) {
        std::cout << "HOSTERROR audio channel '" << mappingName.toStdString()
                  << "' unavailable (err " << audioSection.error() << ")\n";
        std::cout.flush();
        return 1;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(audioSection.data());

    // Open the wake-up channel before scanning for the same reason: a host
    // that cannot be woken must say so instead of spending seconds loading a
    // plugin whose blocks it would answer only on the timer tick.
    pluginHostIpc::NamedEvent requestEvent;
    pluginHostIpc::NamedEvent responseEvent;
    const auto requestName = pluginHostIpc::requestEventNameFor(mappingName);
    const auto responseName = pluginHostIpc::responseEventNameFor(mappingName);
    if (!requestEvent.open(requestName.toWideCharPointer())
        || !responseEvent.open(responseName.toWideCharPointer())) {
        std::cout << "HOSTERROR audio events '" << requestName.toStdString() << "', '"
                  << responseName.toStdString() << "' unavailable (err "
                  << requestEvent.error() << "/" << responseEvent.error() << ")\n";
        std::cout.flush();
        return 1;
    }

    // The host only ever loads the plugin in this throwaway process.
    juce::String scanError;
    const auto descriptions = PluginHost::scanFileInProcess(file, scanError);
    if (descriptions.isEmpty()) {
        std::cout << "HOSTERROR " << scanError.toStdString() << "\n";
        std::cout.flush();
        return 1;
    }

    // Pick the class the app asked for; fall back to the first class.
    juce::PluginDescription chosen = descriptions.getReference(0);
    if (pluginUniqueId != 0) {
        for (const auto& d : descriptions) {
            if (d.uniqueId == pluginUniqueId) {
                chosen = d;
                break;
            }
        }
    }

    juce::String instError;
    auto loaded = PluginHost::instantiate(chosen, file, 48000.0, 256, stateBase64);
    if (loaded.error.isNotEmpty() || loaded.instance == nullptr) {
        std::cout << "HOSTERROR " << (loaded.error.isNotEmpty() ? loaded.error : juce::String("instantiate failed")).toStdString() << "\n";
        std::cout.flush();
        return 1;
    }

    std::cout << "HOSTREADY " << file.getFullPathName().toStdString() << "\n";
    std::cout.flush();

    auto* plugin = loaded.instance.get();
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> scratch(2, pluginHostIpc::kMaxBlockFrames);
    std::uint64_t lastSequence = 0;
    double lastSampleRate = 0.0;
    int lastBufferSize = 0;
    // Initial ratiowarm: the plugin was instantiated at 48 kHz / 256, so a
    // first block that arrives at a different ratio is not a format change
    // the user asked for — it is the app's live device rate. We still
    // re-prepare, but only when the incoming ratio differs from the last
    // one we actually processed, never merely because the block carries a
    // new header value ticking through. That avoids a prepare/dropout on
    // every block when the device buffer size jitters.
    while (true) {
        const auto seq = block->sequence;
        if (seq != 0 && seq != lastSequence) {
            const int frames = juce::jmin(block->numFrames, pluginHostIpc::kMaxBlockFrames);
            const double sr = block->sampleRate > 0.0 ? block->sampleRate : 48000.0;
            const int bs = block->bufferSize > 0 ? block->bufferSize : 256;
            if (sr != lastSampleRate || bs != lastBufferSize) {
                plugin->releaseResources();
                plugin->setRateAndBufferSizeDetails(sr, bs);
                plugin->prepareToPlay(sr, bs);
                lastSampleRate = sr;
                lastBufferSize = bs;
            }
            if (frames > 0 && frames <= pluginHostIpc::kMaxBlockFrames) {
                // Torn-read guard: stable even sequence on both sides.
                const auto before = block->blockSequence;
                for (int i = 0; i < frames; ++i) {
                    scratch.getWritePointer(0)[i] = block->interleave[i * 2];
                    scratch.getWritePointer(1)[i] = block->interleave[i * 2 + 1];
                }
                const auto after = block->blockSequence;
                if (before != after || (before % 2) == 1) {
                    // Torn: skip this block, do not process half-written audio.
                    // Ack it anyway, otherwise the app (which only submits after
                    // an ack) would stop feeding this host forever. The app's
                    // own torn guard rejects the payload and plays dry.
                    block->readSequence = seq;
                } else {
                    block->readSequence = seq;
                    juce::AudioBuffer<float> view(scratch.getArrayOfWritePointers(), 2, 0, frames);
                    midi.clear();
                    plugin->processBlock(view, midi);
                    // Write-back with the same guard: mark odd, copy, mark even.
                    block->blockSequence = block->blockSequence + 1;
                    for (int i = 0; i < frames; ++i) {
                        block->interleave[i * 2] = scratch.getReadPointer(0)[i];
                        block->interleave[i * 2 + 1] = scratch.getReadPointer(1)[i];
                    }
                    block->blockSequence = block->blockSequence + 1;
                }
            }
            block->ackSequence = seq;
            block->responseSequence = seq;
            lastSequence = seq;
            // Wake the app's audio thread; it waits on this event instead of
            // spinning, so the response is not bound to the system timer tick.
            responseEvent.signal();
        }
        // Idle until the app publishes the next block. The timeout is only an
        // idle re-check (e.g. after the app stopped feeding); the wake-up path
        // is the signal, which returns immediately.
        requestEvent.wait(1000);
    }
    return 0;
}
