#pragma once

#include <JuceHeader.h>

#include "NamedEvent.h"
#include "NamedSection.h"

#include <atomic>
#include <thread>

// Runs a VST3 in a separate process (PR-01): the main app only reads plugin
// metadata and audio blocks it sends. The host is this executable with
// --plugin-host. Communication is a shared-memory stereo audio block
// (see PluginHostIpc.h) with a sequence/response handshake, and two named
// events that wake each side when a block is published or acked. Full OS-level
// sandboxing (AppContainer token) is still TODO; the host currently runs
// with the user's token, so only load trusted plugins.
//
// PluginHost no longer launches this for DSP: the instance the rack shows an
// editor for has to be the instance that renders (see PluginHost::processBlock).
// The command-line host mode and its audio channel stay in place for a future
// bridge that can also move the editor into this process.
class RestrictedHost {
public:
    RestrictedHost();
    ~RestrictedHost();

    // Starts the host process for the plugin at `file`. Returns false with
    // `error` set when the process cannot start. PluginHost falls back to
    // the in-process instance when this fails. UniqueId and state are
    // forwarded so multi-class bundles and saved settings resolve in the
    // host process.
    bool launch(const juce::File& file, int pluginUniqueId, const juce::String& stateBase64,
                juce::String& error);
    bool isAlive() const;
    // True once the background watcher has seen the host process exit. Reads
    // only an atomic flag, so the audio thread never polls the OS for it.
    bool isDead() const;
    // Exit code of the host process, for the crash log. Meaningful once the
    // process has finished; 0xC0000005 is an access violation inside a plugin.
    int exitCode() const;
    // Whatever the host wrote to stdout (HOSTREADY / HOSTERROR <reason>).
    // Reads what is buffered; call it once the process has finished.
    juce::String pendingOutput();
    // Sends one JSON control line and returns the host's one-line reply.
    juce::String sendControl(const juce::String& jsonLine, int timeoutMs = 5000);

    bool configureHost(int pluginUniqueId, const juce::String& stateBase64, juce::String& error);
    // Replaces the shared audio block contents with the caller's block and
    // flips the sequence counter, so the host can process it. Returns true
    // when the host was alive and the block was handed over.
    bool submitAudioBlock(const juce::AudioBuffer<float>& input, double sampleRate,
                          int bufferSize);
    // Returns the host's answer for the last submitted block, if any.
    // outRate/outBufferSize echo the header so the caller can verify the
    // host did not drift the format.
    bool fetchAudioBlock(juce::AudioBuffer<float>& output, std::uint64_t expectedSequence,
                         double& outRate, int& outBufferSize);
    // Kills and reaps the host process.
    void killAndReap();

    // Host-side entry point: main() calls this for --plugin-host. Runs the
    // plugin for the whole process lifetime; never returns.
    // sharedMappingName comes from the app: recomputing it from the plugin path
    // on both sides is fragile, and a mismatch made the host bail out with
    // "block == nullptr" after loading the plugin.
    static int runHost(const juce::File& file, int pluginUniqueId = 0,
                       const juce::String& stateBase64 = {},
                       const juce::String& sharedMappingName = {});

private:
    struct HostScratch {
        // Per-slot scratch so a fallback render never mutates the caller's
        // live block. PluginHost keeps one per insert; submit copies from
        // here and fetch writes back here before the caller commits.
        juce::AudioBuffer<float> buffer;
        int capacity = 0;
    };
    HostScratch hostScratch;

    std::unique_ptr<juce::ChildProcess> child;
    pluginHostIpc::NamedSection audioSection;
    // request wakes the host after a block is published; response wakes the
    // audio thread when the host has acked it. Events instead of a 1 ms poll:
    // Windows 11 ignores a background process's timer resolution, which made
    // the host answer on the 15.6 ms tick and stuttered the output.
    pluginHostIpc::NamedEvent requestEvent;
    pluginHostIpc::NamedEvent responseEvent;
    juce::String sharedName;
    juce::String pluginPath;
    int pluginUniqueId = 0;
    bool launched = false;
    bool configured = false;

    // Death is observed on a worker so processBlock only reads hostDead.
    std::atomic<bool> hostDead{false};
    std::atomic<bool> stopWatcher{false};
    std::thread watcher;
    void startWatcher();
    void stopWatcherThread();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RestrictedHost)
};
