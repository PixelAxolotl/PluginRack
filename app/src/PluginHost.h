#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>

// Tracks the plugin file a host child is inside, so a crash can be blamed
// on it, and remembers plugins that crashed a host while loading. A plugin
// that faults in its own DllMain cannot be caught, so a crashed plugin is
// recorded and refused on later loads until the list is cleared.
namespace PluginLoadGuard {
void begin(const juce::File& file);   // any thread
void end();                           // any thread
juce::String currentLoadingPlugin();  // empty when nothing is loading
// Allocation-free snapshot for the crash handler (no JUCE String).
std::size_t currentLoadingPluginBytes(char* dest, std::size_t destBytes);
juce::File blockedListFile();
bool isBlocked(const juce::File& file);
void block(const juce::File& file);
int clearBlocked();                   // returns how many entries were dropped
} // namespace PluginLoadGuard

// Path helpers shared by the scan/host guards (PR-SEC-007/008).
namespace PluginPathUtils {
// Canonical lower-case final path (junctions resolved) for blocklist keys.
juce::String canonicalPluginPath(const juce::File& file);
} // namespace PluginPathUtils

class PluginHost {
public:
    // PR-SEC-001: out-of-process-only. The main app never maps the VST3
    // binary: LoadedPlugin is metadata (path/name/id) resolved from the
    // scan child. The real instance lives only inside --plugin-host
    // (RestrictedHost::runHost calls instantiateHostInstance there).
    struct LoadedPlugin {
        juce::String path;
        juce::String name;
        int uniqueId = 0;
        juce::String error;
    };

    // Host-side instance handle. Only --plugin-host (and --scan-plugin)
    // ever hold one; the main app never calls instantiateHostInstance.
    struct HostInstance {
        std::shared_ptr<juce::AudioPluginInstance> instance;
        juce::String name;
        juce::String error;
    };

    PluginHost();
    ~PluginHost();

    // Reads a bundle's descriptions. The enumeration maps the plugin binary
    // and asks its factory for its classes, which takes seconds for some VST3s
    // (Native Instruments' Raum needs ~3.3 s) and runs in a child process so
    // the message thread never waits and a plugin that faults while scanning
    // cannot take the app down. Any thread.
    static juce::Array<juce::PluginDescription> scanFile(const juce::File& file,
                                                         juce::String& error);

    // The scan itself. Maps the plugin into the calling process: only the
    // --scan-plugin and --plugin-host children run this.
    static juce::Array<juce::PluginDescription> scanFileInProcess(const juce::File& file,
                                                                  juce::String& error);

    // A description as one line of JSON, for the child-process handover.
    static juce::String describeAsJson(const juce::PluginDescription& description);

    // Host-side only: creates the instance inside --plugin-host (or the
    // --scan-plugin child). Never call this in the main app process.
    static HostInstance instantiateHostInstance(const juce::PluginDescription& description,
                                                const juce::File& file, double sampleRate,
                                                int bufferSize,
                                                const juce::String& stateBase64 = {});

    // scanFile plus metadata resolve, for callers already on the message
    // thread. Returns path/name/id only — no instance is created here.
    // pluginUniqueId selects the class inside a multi-class bundle; 0 falls
    // back to the bundle's first class. The returned path is the bundle
    // root: when the input names the inner binary
    // (Contents\x86_64-win\*.vst3) it is walked up to the enclosing
    // *.vst3 folder, because instantiating the inner binary directly
    // crashes the host after launch.
    static LoadedPlugin loadFromFile(const juce::File& file, double sampleRate,
                                     int bufferSize, const juce::String& stateBase64 = {},
                                     int pluginUniqueId = 0);
    // Message thread: records the metadata and starts the sandboxed host.
    // Fail-closed: when the host cannot start, the slot stays silent with a
    // visible badge (hostDown) instead of rendering in-process.
    void adopt(const LoadedPlugin& loaded, const juce::String& stateBase64 = {},
               int pluginUniqueId = 0);

    // stateBase64 is an opaque blob previously returned by getStateAsBase64;
    // when non-empty it is applied before the plugin is prepared to play, so
    // a restored slot comes back with the settings it was saved with.
    // pluginUniqueId names the class inside a multi-class bundle.
    juce::String loadPlugin(const juce::File& file, const juce::String& stateBase64 = {},
                            int pluginUniqueId = 0);
    // Captures the plugin's own state via the host control channel
    // (parameters, programs) as base64 text. Empty when no plugin is loaded
    // or the plugin exposes no state.
    juce::String getStateAsBase64() const;
    void prepareToPlay(double sampleRate, int samplesPerBlock);
    void releaseResources();
    void processBlock(juce::AudioBuffer<float>& buffer, bool bypassed = false);

    juce::String getPluginPath() const;
    juce::String getPluginName() const;
    double getSampleRate() const;
    int getBufferSize() const;

    bool hasPlugin() const;
    // True when the sandboxed host is alive and answering. The strip badge
    // reads this: a loaded-but-dead slot shows "host down" and stays silent.
    bool isHostAlive() const;
    bool isHostDown() const;
    // True once a launch has ever succeeded for this slot. The health
    // watchdog uses it to tell "still handshaking" from "died after
    // launch" during startup warm-up.
    bool wasEverReady() const;
    // Whether a host object exists (launch attempted). Lets the watchdog
    // tell "never launched" from "launched then died".
    bool hasHost() const;
    // Exit code of the host child the last time it was observed dead
    // (STILL_ACTIVE while alive). Lets the watchdog log HOW the host died.
    unsigned long lastHostExitCode() const;
    // Editor window for bay embedding (see RestrictedHost): asks the host
    // for its editor HWND + size, 0 when the host is down or the plugin
    // has no editor. Message thread only.
    unsigned long long requestEditorWindow(int& widthOut, int& heightOut);
    // Why the last editor request failed (see RestrictedHost). Empty on success.
    juce::String lastEditorFailReason() const;
    // Asks the host to apply the GH bay fit scale to the editor content
    // (see RestrictedHost). Message thread only; no-op when the host is down.
    void sendEditorScale(float scale);
    // Asks the host for the editor's current outer size (see
    // RestrictedHost). Message thread only; false when unknown.
    bool requestEditorSize(int& widthOut, int& heightOut);
    // Returns the embedded window to the host. Message thread only.
    void releaseEditorWindow();
    // Lock-free cached latency: refreshed on load/prepare/bypass so the
    // latency report never blocks on pluginLock.
    int getLatencySamples() const;
    void refreshLatencyCache();
    // The editor runs in the host process and is shown as a native child
    // window reparented into the dock bay (see NativeThumbnail helpers).
    // These return null / no-op until the host reports its editor window.
    void showEditor();
    // Channel-view docking: asks the host for its editor window handle and
    // embeds it inline. Returns null when the host has no editor; destroy
    // with destroyDockEditor.
    juce::AudioProcessorEditor* createEditorForDock();
    void destroyDockEditor(juce::AudioProcessorEditor* editor);
    void closeFloatingEditor();
    void editorBeingDeleted(juce::AudioProcessorEditor* editor);
    void clearEditorWindow(juce::DocumentWindow* window);
    void closeEditorWindow(juce::DocumentWindow* window);

private:
    mutable juce::CriticalSection pluginLock;
    juce::String pluginPath;
    juce::String pluginName;
    int pluginUniqueId = 0;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 256;
    bool prepared = false;
    std::atomic<int> cachedLatencySamples_{0};

    // PR-SEC-001: out-of-process-only. There is no in-process instance;
    // audio renders in the sandboxed host or the slot stays silent.
    std::unique_ptr<class RestrictedHost> host;
    std::uint64_t hostSequence = 0;
    bool hostReady = false;
    // Latched on the first successful launch. The health watchdog uses it
    // to tell "still handshaking" from "died after launch" during startup.
    bool everReady = false;
    // Seamless-add scratch: the live output block is never handed to the
    // host IPC directly. Submit copies the range here; fetch writes the
    // host's answer here; only on a clean ack do we copy it back into the
    // live buffer. A dead host leaves the live buffer untouched (dry).
    juce::AudioBuffer<float> hostScratch;
    int hostScratchCapacity = 0;

    void startHostLocked(const juce::File& file, int uniqueId, const juce::String& stateBase64,
                         juce::String& error);
    void stopHostLocked();
    // Same as refreshLatencyCache but assumes pluginLock is already held
    // (adopt calls it under the lock; the public one takes a try-lock).
    void refreshLatencyCacheLocked();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHost)
};
