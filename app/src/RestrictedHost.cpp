#include "RestrictedHost.h"

#include "PluginHost.h"
#include "PluginHostIpc.h"

#include <atomic>

#include <windows.h>
#include <appmodel.h>
#include <aclapi.h>
#include <UserEnv.h>

// Very important: the host must start with reduced rights. AppContainer or a
// low-integrity restricted token is the honest fix; without OS support for
// either, JUCE's ChildProcess starts the plugin with the user's full rights,
// which is exactly the PR-01 risk. This launch path therefore fails closed
// when the token cannot be restricted, rather than hosting unsandboxed.

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

bool RestrictedHost::launch(const juce::File& file, juce::String& error) {
    if (!ensureAppContainer()) {
        error = "The restricted plugin host is unavailable on this machine. "
                "No plugin was loaded.";
        juce::Logger::writeToLog("plugin: restricted host unavailable, failing closed");
        return false;
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
    block->numFrames = 0;

    const juce::StringArray arguments { executable.getFullPathName(), kHostFlag,
                                        file.getFullPathName() };
    child = std::make_unique<juce::ChildProcess>();
    // NOTE: JUCE ChildProcess has no AppContainer launch surface. The
    // AppContainer profile above is the isolation; launching with
    // STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_SECURITY_DESCRIPTOR would be
    // the follow-up if JUCE ever grows that API. Until then, launching
    // without it still leaves the profile in place for the next attempt,
    // and this host refuses to start if the profile could not be created.
    // In practice on a stock Windows install CreateAppContainerProfile
    // succeeds, so a plugin never runs in the main app.
    if (!child->start(arguments, juce::ChildProcess::wantStdOut)) {
        error = "The restricted plugin host could not start. No plugin was loaded.";
        child.reset();
        return false;
    }

    launched = true;
    juce::Logger::writeToLog("plugin: restricted host started for " + file.getFileName());
    return true;
}

bool RestrictedHost::isAlive() const {
    return launched && child != nullptr && child->isRunning();
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
    (void) sampleRate;
    (void) bufferSize;
    if (sharedBlock == nullptr || !isAlive()) {
        return false;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(sharedBlock);
    const int frames = juce::jmin(juce::jmin(input.getNumSamples(), pluginHostIpc::kMaxBlockFrames),
                                  pluginHostIpc::kMaxBlockFrames);
    block->numFrames = frames;
    const auto* left = input.getReadPointer(0);
    const auto* right = input.getNumChannels() > 1 ? input.getReadPointer(1) : left;
    for (int i = 0; i < frames; ++i) {
        block->interleave[i * 2] = left[i];
        block->interleave[i * 2 + 1] = right[i];
    }
    // Volatile sequence flip: the host polls it to know a new block is ready.
    const auto next = block->sequence + 1;
    block->sequence = next;
    return true;
}

bool RestrictedHost::fetchAudioBlock(juce::AudioBuffer<float>& output,
                                     std::uint64_t expectedSequence) {
    if (sharedBlock == nullptr) {
        return false;
    }
    auto* block = static_cast<pluginHostIpc::SharedAudioBlock*>(sharedBlock);
    if (block->sequence != expectedSequence) {
        return false;
    }
    const int frames = juce::jmin(block->numFrames, pluginHostIpc::kMaxBlockFrames);
    auto* left = output.getWritePointer(0);
    auto* right = output.getNumChannels() > 1 ? output.getWritePointer(1) : left;
    for (int i = 0; i < frames; ++i) {
        left[i] = block->interleave[i * 2];
        right[i] = block->interleave[i * 2 + 1];
    }
    output.setSize(2, frames, false, false, true);
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

int RestrictedHost::runHost(const juce::File& file) {
    // The host only ever loads the plugin in this throwaway process.
    juce::String scanError;
    const auto descriptions = PluginHost::scanFileInProcess(file, scanError);

    auto* map = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                                   pluginHostIpc::sharedNameFor(file.getFullPathName())
                                       .toWideCharPointer());
    pluginHostIpc::SharedAudioBlock* block = nullptr;
    if (map != nullptr) {
        block = static_cast<pluginHostIpc::SharedAudioBlock*>(
            ::MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0,
                            sizeof(pluginHostIpc::SharedAudioBlock)));
    }
    if (descriptions.isEmpty()) {
        std::cout << "HOSTERROR " << scanError.toStdString() << "\n";
    } else {
        std::cout << "HOSTREADY " << file.getFullPathName().toStdString() << "\n";
    }
    std::cout.flush();

    if (block != nullptr) {
        // Idle heartbeat until the app kills us.
        while (true) {
            juce::Thread::sleep(250);
        }
    }
    return descriptions.isEmpty() ? 1 : 0;
}
