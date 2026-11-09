#pragma once

#include <JuceHeader.h>

// Runs a VST3 in a separate process (PR-01): the main app only reads plugin
// metadata and audio blocks it sends. The host is this executable with
// --plugin-host. Communication is a shared-memory stereo audio block
// (see PluginHostIpc.h) with a sequence/response handshake. Full OS-level
// sandboxing (AppContainer token) is still TODO; the host currently runs
// with the user's token, so only load trusted plugins.
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
    static int runHost(const juce::File& file, int pluginUniqueId = 0,
                       const juce::String& stateBase64 = {});

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
    void* sharedBlock = nullptr;
    juce::String sharedName;
    juce::String pluginPath;
    int pluginUniqueId = 0;
    bool launched = false;
    bool configured = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RestrictedHost)
};
