#pragma once

#include <JuceHeader.h>

// Runs a VST3 in a separate process: the main app never loads the plugin
// binary itself for audio — the host is this executable with --plugin-host.
// Communication is a per-launch random-named shared-memory stereo audio block
// (see PluginHostIpc.h) with a sequence/response handshake, plus a named-pipe
// control channel that carries the preset state and parameter updates.
// The child runs in an AppContainer (low-integrity fallback); when neither
// containment is available the load is refused rather than run in-process.
class RestrictedHost {
public:
    RestrictedHost();
    ~RestrictedHost();

    // Starts the host process for the plugin at `file`. Returns false with
    // `error` set when the process cannot start or cannot be contained.
    // Fail-closed: PluginHost must NOT fall back to an in-process instance.
    // UniqueId selects the class in multi-class bundles; the state blob is
    // delivered over the control pipe after the handshake, never on argv.
    bool launch(const juce::File& file, int pluginUniqueId, const juce::String& stateBase64,
                juce::String& error);
    bool isAlive() const;
    // Exit code of the child the last time it was observed dead (STILL_ACTIVE
    // when alive). Lets the watchdog log HOW the host died, not just that it
    // did. Any thread; takes no locks.
    unsigned long lastChildExitCode() const;
    // Sends one JSON control line and returns the host's one-line reply.
    juce::String sendControl(const juce::String& jsonLine, int timeoutMs = 5000);
    // Editor window for bay embedding: asks the host to create its editor
    // (if the plugin has one) and report the native HWND + size. Returns
    // 0 when the host is down or the plugin has no editor. The window stays
    // owned by the host process; the bay reparents it with SetParent (see
    // RackView) and must call releaseEditorWindow before the host dies.
    // Message thread only: the control round-trip blocks up to timeoutMs.
    unsigned long long requestEditorWindow(int& widthOut, int& heightOut,
                                           int timeoutMs = 5000);
    // Reason the last editor request failed ("timeout", "no-reply", or the
    // child's own reason such as "createEditor-null"). For the log line in
    // RackView::refreshEditor; empty when the last request succeeded.
    juce::String lastEditorFailReason() const;
    // Asks the host to apply the GH bay fit scale to the editor content
    // (see setEditorScale). Fire-and-forget; message thread only.
    void sendEditorScale(float scale);
    // Asks the host for the editor's CURRENT outer size (logical units).
    // The watchdog uses it to tell a plugin that grew itself from a host
    // that hasn't caught up yet. Returns false when unknown.
    bool requestEditorSize(int& widthOut, int& heightOut);
    // Returns the embedded window to the host's hidden parent. Message
    // thread only; no-op when the host is down.
    void releaseEditorWindow();
    // Destroys the host-side editor (frees its resources). Message thread
    // only; no-op when the host is down.
    void dropEditorWindow();

    // Re-sends state/unique-id over the control pipe after launch (used when
    // the launch-time delivery failed but the host is alive). Prefer launch()
    // for the initial delivery; calling this twice duplicates the state line.
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
    // plugin for the whole process lifetime; never returns. The shared
    // section name and the control pipe name arrive on argv (per-launch
    // random values chosen by the parent); the preset state arrives over
    // the control pipe, never on the command line.
    static int runHost(const juce::File& file, int pluginUniqueId = 0,
                       const juce::String& sharedName = {},
                       const juce::String& controlPipeName = {});

private:
    // Control pipe (parent write / child read): one JSON line per message,
    // one JSON line reply. Carries the preset state and parameter updates.
    void* controlPipe = nullptr;
    juce::String controlPipeName;
    juce::String pendingStateBase64;

    void* jobObject = nullptr;
    void* childProcessHandle = nullptr;
    unsigned long childPid = 0;

    std::unique_ptr<juce::ChildProcess> child;
    void* sharedBlock = nullptr;
    // Retained section handle: keeps the named section published for the
    // child's OpenFileMappingW for the whole host lifetime. Closed in
    // killAndReap. (Closing the last handle can unpublish the name even
    // while our view stays mapped — the child then misses with "file not
    // found" and the launch fails.)
    void* sectionHandle = nullptr;
    juce::String sharedName;
    juce::String pluginPath;
    int pluginUniqueId = 0;
    bool launched = false;
    bool configured = false;
    // Mutable: set by the const request path for the log line.
    mutable juce::String lastEditorFailReason_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RestrictedHost)
};
