#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>

#include "PluginProcessHost.h"

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
    // The instance renders in this process, so its editor and parameters are
    // the ones the rack shows and edits. UniqueId and state are accepted for
    // the callers that already resolved them; loadFromFile selected the class
    // by id and instantiate applied the state before adoption.
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
    // Renders the instance that owns the editor and the parameters the rack
    // exposes. A helper process would receive the audio instead, leaving the
    // visible plugin with dead meters and every edit inaudible, so this class
    // never hands a block to another process.
    void processBlock(juce::AudioBuffer<float>& buffer, bool bypassed = false);

    // --- Fault containment: child-process DSP ------------------------------
    // Off unless loadChildHost() is called. Once a slot is published its
    // renderer is fixed: a launch failure leaves it in-process (the caller just
    // does not call loadChildHost), and a later child death leaves it DRY until
    // the slot is rebuilt. Nothing here ever switches a live slot to the
    // in-process instance; see
    // docs/superpowers/specs/2026-11-13-fault-containment-ipc.md §3.2.
    struct ChildHostStatus {
        bool enabled = false;
        bool failed = false;
        int exitCode = 0;
        int missedBlocks = 0;
    };

    // Spawns the child, waits for HOSTREADY, pushes the saved state and the
    // current format. Worker thread only (it waits for the plugin to load).
    // Returns false with `error` set; the caller then publishes an in-process
    // slot, which is a launch failure, not a degraded child slot.
    bool loadChildHost(const juce::File& file, int pluginUniqueId, const juce::String& stateBase64,
                       double sampleRate, int bufferSize, int readyTimeoutMs,
                       juce::String& error);
    // Takes over a child that a worker already spawned and handshook, so the
    // insert load path never blocks the message thread on plugin loading.
    // Pushes state and the format; returns false if the child cannot take them.
    bool adoptChildHost(std::unique_ptr<PluginProcessHost> host, const juce::String& stateBase64,
                        double sampleRate, int bufferSize, int timeoutMs, juce::String& error);
    // Mirrors an editor-side parameter change into the child. The value is
    // normalised (0..1), the same domain the plugin reports.
    //
    // PARKED: no caller yet (13 Nov 2026). This is the Phase 2 entry point
    // (plan task 2.1). Do not wire it before the four fixes in plan §6.0 -
    // forwarding edits into the child while the renderer can still fall through
    // to the in-process instance was part of the configuration that crashed.
    bool setChildParameter(int parameterIndex, float normalisedValue);
    void stopChildHost();
    bool childHostEnabled() const { return childHost_ != nullptr; }
    ChildHostStatus childHostStatus() const;
    int childProcessId() const;
    PluginProcessHost::Peaks childPeaks() const;

    juce::String getPluginPath() const;
    juce::String getPluginName() const;
    double getSampleRate() const;
    int getBufferSize() const;

    bool hasPlugin() const;
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

    // Child-process DSP state. childScratch_ is sized in prepareToPlay and is
    // the only buffer the audio thread hands to the child, so a failed block
    // can never have mutated the caller's live audio.
    std::unique_ptr<PluginProcessHost> childHost_;
    juce::AudioBuffer<float> childScratch_;
    std::atomic<double> playSampleRate_{48000.0};
    std::atomic<int> playBufferSize_{256};
    std::atomic<int> childBlockDeadlineMs_{10};
    std::atomic<int> childConsecutiveMisses_{0};
    std::atomic<bool> childFailed_{false};
    mutable juce::String childLastKnownState_;

    void noteChildMiss();

    juce::DocumentWindow* editorWindow = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHost)
};
