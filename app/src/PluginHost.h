#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>

// Tracks the plugin file currently inside plugin code so a crash can be blamed
// on it, and remembers plugins that crashed the app while loading. A plugin
// that faults in its own DllMain cannot be caught in-process, so a crashed
// plugin is recorded and refused on later loads until the list is cleared.
namespace PluginLoadGuard {
void begin(const juce::File& file);   // any thread
void end();                           // any thread
juce::String currentLoadingPlugin();  // empty when nothing is loading
juce::File blockedListFile();
bool isBlocked(const juce::File& file);
void block(const juce::File& file);
int clearBlocked();                   // returns how many entries were dropped
} // namespace PluginLoadGuard

class PluginHost {
public:
    // Result of instantiating a VST3 file. The instance is shared so it can be
    // built on a worker thread (instantiation blocks for as long as the plugin
    // takes to initialise) and then adopted on the message thread, where
    // prepareToPlay must run: some plugins synchronise with it.
    struct LoadedPlugin {
        std::shared_ptr<juce::AudioPluginInstance> instance;
        juce::String path;
        juce::String name;
        juce::String error;
    };

    PluginHost();
    ~PluginHost();

    // Reads a bundle's descriptions. The enumeration loads the plugin binary and
    // asks its factory for its classes, which takes seconds for some VST3s
    // (Native Instruments' Raum needs ~3.3 s) and runs in a child process so the
    // message thread never waits and a plugin that faults while scanning cannot
    // take the app down. Any thread.
    static juce::Array<juce::PluginDescription> scanFile(const juce::File& file,
                                                         juce::String& error);

    // The scan itself. Loads the plugin into the calling process: only the
    // --scan-plugin child process uses this.
    static juce::Array<juce::PluginDescription> scanFileInProcess(const juce::File& file,
                                                                  juce::String& error);

    // A description as one line of JSON, for the child-process handover.
    static juce::String describeAsJson(const juce::PluginDescription& description);

    // Creates the instance for an already scanned description and configures its
    // buses and state. Message thread only: JUCE creates instances there, and
    // loading the plugin's DLL on one thread and using it on another faults
    // inside some VST3s (Native Instruments' Raum and Replika).
    static LoadedPlugin instantiate(const juce::PluginDescription& description,
                                    const juce::File& file, double sampleRate, int bufferSize,
                                    const juce::String& stateBase64 = {});

    // scanFile plus instantiate, for callers already on the message thread.
    // pluginUniqueId selects the class inside a multi-class bundle; 0 falls
    // back to the bundle's first class.
    static LoadedPlugin loadFromFile(const juce::File& file, double sampleRate,
                                     int bufferSize, const juce::String& stateBase64 = {},
                                     int pluginUniqueId = 0);
    // Message thread: takes over the instance and prepares it to play.
    // UniqueId and state are forwarded to the restricted host so saved
    // settings and multi-class bundles resolve there.
    void adopt(const LoadedPlugin& loaded, const juce::String& stateBase64 = {},
               int pluginUniqueId = 0);

    // stateBase64 is an opaque blob previously returned by getStateAsBase64;
    // when non-empty it is applied before the plugin is prepared to play, so
    // a restored slot comes back with the settings it was saved with.
    // pluginUniqueId names the class inside a multi-class bundle.
    juce::String loadPlugin(const juce::File& file, const juce::String& stateBase64 = {},
                            int pluginUniqueId = 0);
    // Captures the plugin's own state (parameters, programs) as base64 text.
    // Empty when no plugin is loaded or the plugin exposes no state.
    juce::String getStateAsBase64() const;
    void prepareToPlay(double sampleRate, int samplesPerBlock);
    void releaseResources();
    void processBlock(juce::AudioBuffer<float>& buffer, bool bypassed = false);

    juce::String getPluginPath() const;
    juce::String getPluginName() const;
    double getSampleRate() const;
    int getBufferSize() const;

    bool hasPlugin() const;
    // True when a restricted host was configured for this plugin but has
    // since died: the insert renders dry and its UI is not worth hosting.
    bool hostLost() const;
    // The hosted instance (message thread only, may be null). The caller must
    // not keep it across insert removal - the rack's generic parameter panel
    // re-reads it on every refresh for exactly that reason.
    juce::AudioPluginInstance* getInstance() const;
    // Lock-free cached latency: refreshed on load/prepare/bypass so the
    // latency report never blocks on pluginLock.
    int getLatencySamples() const;
    void refreshLatencyCache();
    void showEditor();
    // Channel-view docking: create an editor the caller owns and embeds inline
    // (addAndMakeVisible). Hides any floating window first. Returns null when
    // the plugin has no editor; destroy with destroyDockEditor.
    juce::AudioProcessorEditor* createEditorForDock();
    void destroyDockEditor(juce::AudioProcessorEditor* editor);
    void closeFloatingEditor();
    void editorBeingDeleted(juce::AudioProcessorEditor* editor);
    void clearEditorWindow(juce::DocumentWindow* window);
    void closeEditorWindow(juce::DocumentWindow* window);

private:
    std::shared_ptr<juce::AudioPluginInstance> plugin;
    mutable juce::CriticalSection pluginLock;
    juce::MidiBuffer midiBuffer;
    juce::String pluginPath;
    juce::String pluginName;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 256;
    bool prepared = false;
    std::atomic<int> cachedLatencySamples_{0};

    juce::DocumentWindow* editorWindow = nullptr;

    // PR-01: audio renders in a restricted host process when it starts.
    // If the host cannot start, processBlock falls back to the in-process
    // instance so the slot still sounds (degraded sandbox, not silence).
    std::unique_ptr<class RestrictedHost> host;
    std::uint64_t hostSequence = 0;
    bool hostReady = false;
    // Set once when a configured host is found dead, so the dry-block path
    // reports it instead of silently muting the insert.
    bool hostLossLogged = false;
    // Seamless-add scratch: the live output block is never handed to the
    // host IPC directly. Submit copies the range here; fetch writes the
    // host's answer here; only on a clean ack do we copy it back into the
    // live buffer. A fallback render then finds the live buffer untouched.
    juce::AudioBuffer<float> hostScratch;
    int hostScratchCapacity = 0;

    void startHostLocked(const juce::File& file, int uniqueId, const juce::String& stateBase64,
                         juce::String& error);
    void stopHostLocked();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHost)
};
