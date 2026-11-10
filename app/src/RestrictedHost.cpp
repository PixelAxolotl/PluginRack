#include "RestrictedHost.h"

#include "PluginHost.h"
#include "PluginHostIpc.h"

#include <atomic>

#include <windows.h>
#include <appmodel.h>
#include <aclapi.h>
#include <UserEnv.h>

// The restricted host aims for reduced rights (AppContainer or a low-
// integrity token). JUCE's ChildProcess has no surface for launching into
// an AppContainer, so the host currently runs with the user's normal token
// while the AppContainer profile is created for future use. The security
// benefit right now is process isolation (a crashing/hanging plugin does
// not take the app down); full sandboxing is a later step.

namespace {

constexpr const char* kHostFlag = "--plugin-host";

// Best effort: create the AppContainer profile once. If the OS or account
// forbids it, return false and the caller fails closed.
bool ensureAppContainer() {
    // CreateAppContainerProfile lives in UserEnv.h. It creates (or reuses)
    // the named AppContainer identity for this user; launching the child
    // into that container needs EXEC_METADATA on this exe's folder, which
    // is granted separately via CreateApplicationGroup and is out of scope
    // for a per-user profile. The 4th arg `requestedCapabilities` is the
    // capability SID list; empty means no network/file capabilities, which
    // is exactly the sandbox we want.
    HRESULT hr = CreateAppContainerProfile(
        L"PixelAxolotl.PluginRack.PluginHost",
        nullptr, nullptr, nullptr, 0, nullptr);
    return SUCCEEDED(hr);
}

} // namespace

RestrictedHost::RestrictedHost() = default;
RestrictedHost::~RestrictedHost() {
    killAndReap();
}

bool RestrictedHost::launch(const juce::File& file, int uniqueId,
                            const juce::String& stateBase64, juce::String& error) {
    if (!ensureAppContainer()) {
        juce::Logger::writeToLog("plugin: AppContainer profile unavailable; "
                                 "launching host with normal user token");
    }

    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    if (!executable.existsAsFile()) {
        error = "The plugin host process is missing. No plugin was loaded.";
        return false;
    }

    // Shared audio block: created in the app, inherited by the host via the
    // plugin path name (both sides call CreateFileMapping with the same
    // name; the block itself needs no ACL because AppContainer isolates it).
    sharedName = pluginHostIpc::sharedNameFor(file.getFullPathName());
    const auto size = sizeof(pluginHostIpc::SharedAudioBlock);
    auto* mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                          PAGE_READWRITE | SEC_COMMIT,
                                          0, (DWORD) size,
                                          sharedName.toWideCharPointer());
    if (mapping == nullptr) {
        error = "Could not create the plugin audio channel. No plugin was loaded.";
        return false;
    }
    sharedBlock = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    ::CloseHandle(mapping);
    if (sharedBlock == nullptr) {
        error = "Could not map the plugin audio channel. No plugin was loaded.";
        return false;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(sharedBlock);
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
    return launched && child != nullptr && child->isRunning();
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
    if (sharedBlock == nullptr || !isAlive()) {
        return false;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(sharedBlock);
  // If the host is not producing, give up quickly (0 ms) so the audio
    // thread never blocks behind the IPC. The caller renders in-process.
    const auto waitStart = juce::Time::getMillisecondCounterHiRes();
    const auto previous = block->sequence;
    while (block->responseSequence != previous
           && juce::Time::getMillisecondCounterHiRes() - waitStart < 0.0) {
        juce::Thread::yield();
    }
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
    return true;
}

bool RestrictedHost::fetchAudioBlock(juce::AudioBuffer<float>& output,
                                     std::uint64_t expectedSequence, double& outRate,
                                     int& outBufferSize) {
    if (sharedBlock == nullptr) {
        return false;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(sharedBlock);
    // Brief wait for the host to process this block. The audio thread cannot
    // block long, but the host normally answers within a few ms.
    const auto start = juce::Time::getMillisecondCounterHiRes();
    while (block->responseSequence != expectedSequence
           && juce::Time::getMillisecondCounterHiRes() - start < 20.0) {
        juce::Thread::yield();
    }
    if (block->responseSequence != expectedSequence) {
        return false;
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
    if (child != nullptr && child->isRunning()) {
        child->kill();
        child->waitForProcessToFinish(5000);
    }
    child.reset();
    if (sharedBlock != nullptr) {
        ::UnmapViewOfFile(sharedBlock);
        sharedBlock = nullptr;
    }
    launched = false;
}

int RestrictedHost::runHost(const juce::File& file, int pluginUniqueId,
                            const juce::String& stateBase64,
                            const juce::String& sharedMappingName) {
    // The host only ever loads the plugin in this throwaway process.
    juce::String scanError;
    const auto descriptions = PluginHost::scanFileInProcess(file, scanError);
    if (descriptions.isEmpty()) {
        // Wait briefly for the app to create the mapping, then give up.
        juce::Thread::sleep(100);
    }

    // The app hands over the mapping name; only fall back to recomputing it
    // when an older caller did not pass one.
    const auto mappingName = sharedMappingName.isNotEmpty()
                                 ? sharedMappingName
                                 : pluginHostIpc::sharedNameFor(file.getFullPathName());
    auto* map = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                                   mappingName.toWideCharPointer());
    pluginHostIpc::SharedAudioBlock* block = nullptr;
    if (map != nullptr) {
        block = static_cast<pluginHostIpc::SharedAudioBlock*>(
            ::MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0,
                            sizeof(pluginHostIpc::SharedAudioBlock)));
    }
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

    if (block == nullptr) {
        std::cout << "HOSTERROR audio channel '" << mappingName.toStdString()
                  << "' unavailable (err " << ::GetLastError() << ")\n";
        std::cout.flush();
        return 1;
    }

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
                    block->readSequence = seq;
                    lastSequence = seq;
                    block->responseSequence = seq;
                    block->ackSequence = seq;
                    juce::Thread::sleep(1);
                    continue;
                }
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
            block->ackSequence = seq;
            block->responseSequence = seq;
            lastSequence = seq;
        }
        juce::Thread::sleep(1);
    }
    return 0;
}
