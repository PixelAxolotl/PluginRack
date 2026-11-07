#pragma once

#include <JuceHeader.h>

// Runs a VST3 in a separate process with reduced rights (PR-01): the main
// app can only read plugin metadata and audio it is sent, so a plugin cannot
// touch the user's files or the network. The host is this executable with
// --plugin-host; it exists so the plugin's own code never enters the main
// app. Communication is a length-prefixed JSON control channel plus a
// shared-memory stereo audio block (see PluginHostIpc.h).
class RestrictedHost {
public:
    RestrictedHost();
    ~RestrictedHost();

    // Starts the host process for the plugin at `file`. Returns false with
    // `error` set when the process cannot start; never falls back to
    // in-process hosting.
    bool launch(const juce::File& file, juce::String& error);
    bool isAlive() const;
    // Sends one JSON control line and returns the host's one-line reply.
    juce::String sendControl(const juce::String& jsonLine, int timeoutMs = 5000);

    // Replaces the shared audio block contents with the caller's block and
    // flips the sequence counter, so the host can process it. Returns true
    // when the host was alive and the block was handed over.
    bool submitAudioBlock(const juce::AudioBuffer<float>& input, double sampleRate,
                          int bufferSize);
    // Returns the host's answer for the last submitted block, if any.
    bool fetchAudioBlock(juce::AudioBuffer<float>& output, std::uint64_t expectedSequence);
    // Kills and reaps the host process.
    void killAndReap();

    // Host-side entry point: main() calls this for --plugin-host. Runs the
    // plugin for the whole process lifetime; never returns.
    static int runHost(const juce::File& file);

private:
    std::unique_ptr<juce::ChildProcess> child;
    void* sharedBlock = nullptr;
    juce::String sharedName;
    bool launched = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RestrictedHost)
};
