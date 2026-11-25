#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>

#include "PluginProcessHost.h"
#include "PluginParamMirror.h"

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

// Records plugin faults that killed a child host, persisted in faults.json.
// Unlike the load blocklist (which refuses to load), fault records only warn.
namespace PluginFaultGuard {
juce::File faultsFile();
// Appends one record (idempotent on path within a short window is not required;
// the caller dedupes via InsertSlot::faultRecorded). Returns true on success.
bool record(const juce::String& path, int exitCode, std::uint32_t insertId);
int clear(); // returns how many entries dropped
} // namespace PluginFaultGuard

class PluginHost : private juce::AudioProcessorParameter::Listener, private juce::Timer {
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
        // 2 when a side-chain input bus was enabled at instantiate time, 0
        // when the plugin offers none or refused it. processBlock uses this
        // to decide whether a four-channel composite is legal.
        int sideChainChannels = 0;
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
    // Side-chained render: channels 2 and 3 of the composite are the side-chain
    // input. A plugin without an enabled side-chain bus ignores the pointers
    // and renders the main stereo block, so the caller can always pass them.
    void processBlock(juce::AudioBuffer<float>& buffer, bool bypassed, const float* sideChainL,
                      const float* sideChainR, int sideChainFrames);

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
    // Wired since 25 Nov 2026: while a child is published, the message-thread
    // timer drains the edits parameterValueChanged recorded into paramMirror_
    // and pushes them here; a full ring or a gap the child counted is repaired
    // with a full state push (scheduleParamResync). The listener itself never
    // pushes - it may run on the plugin's audio thread and the ring is
    // single-producer - so the audio thread only pays one atomic store per edit.
    bool setChildParameter(int parameterIndex, float normalisedValue);
    // Unpublishes the child and kills it. Only safe once the audio thread can no
    // longer reach this host: killAndReap() closes the Win32 events and unmaps
    // the shared views submitBlock/fetchBlock use. A caller that has not
    // unlinked the slot first must use detachChildHost().
    void stopChildHost();
    // Unpublishes the child without reaping it and returns the old pointer, so
    // the caller decides *when* the killAndReap() is safe. The audio thread
    // reads childHost_ with acquire and only dereferences the pointer it got,
    // so after this exchange it either holds nothing from this host or holds a
    // shared_ptr the caller owns too; a caller that detaches while holding the
    // engine stateLock (render() holds it across processBlock) knows the audio
    // thread is out of submitBlock/fetchBlock before it reaps. Null when the
    // slot had no child.
    std::shared_ptr<PluginProcessHost> detachChildHost();
    bool childHostEnabled() const {
        return childHost_.load(std::memory_order_acquire) != nullptr;
    }
    // Renderer mode is fixed for the life of the slot (spec §3.2). 0 = in-process, 1 = child.
    int rendererMode() const { return rendererMode_.load(std::memory_order_acquire); }
    ChildHostStatus childHostStatus() const;
    int childProcessId() const;

    double getSampleRate() const;
    int getBufferSize() const;

    bool hasPlugin() const;
    // The hosted instance (message thread only, may be null). The caller must
    // not keep it across insert removal - the rack's generic parameter panel
    // re-reads it on every refresh for exactly that reason.
    juce::AudioPluginInstance* getInstance() const;
    // Monotonic count of parameter edits made since the instance was adopted.
    // The autosave uses it to notice knobs turned inside a plugin editor
    // without capturing the (comparatively expensive) plugin state blob.
    std::uint64_t getParameterRevision() const {
        return parameterRevision_.load(std::memory_order_relaxed);
    }
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
    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override;
    void attachParameterListeners();
    void detachParameterListeners();

    // Child parameter forwarding (see setChildParameter). The listener records
    // into paramMirror_; this timer is the only producer into the IPC ring, so
    // both are message thread and the ring's single-producer contract holds.
    void timerCallback() override;
    void pushPendingChildParameters();
    void startChildParamForwarding();
    void stopChildParamForwarding();
    PluginParamMirror paramMirror_;
    // Baseline for the gap counter the child maintains: a new value means it
    // dropped an edit while draining, which only a full state push repairs.
    std::uint64_t observedParamGapCount_ = 0;

    std::shared_ptr<juce::AudioPluginInstance> plugin;
    mutable juce::CriticalSection pluginLock;
    std::atomic<std::uint64_t> parameterRevision_{0};
    juce::MidiBuffer midiBuffer;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 256;
    bool prepared = false;
    std::atomic<int> cachedLatencySamples_{0};

    // Child-process DSP state. childScratch_ is sized in prepareToPlay and is
    // the only buffer the audio thread hands to the child, so a failed block
    // can never have mutated the caller's live audio.
    // Published atomically so the audio thread can never observe a
    // half-torn pointer (fix 2 of the resume gate). A slot whose renderer
    // mode is "child" keeps reading dry when this is null.
    //
    // Two ways out, and the difference is the caller's lock discipline:
    // stopChildHost() unpublishes *and* reaps in one call, so it requires that
    // the audio thread can no longer reach this slot (the insert is unlinked,
    // or the audio device is stopped); detachChildHost() only unpublishes and
    // hands the caller the pointer, which is what a reload needs because the
    // slot stays live while its child is replaced.
    std::atomic<std::shared_ptr<PluginProcessHost>> childHost_{};
    std::atomic<int> rendererMode_{0}; // 0 = in-process, 1 = child (fix 1)
    juce::AudioBuffer<float> childScratch_;
    // Four-channel working buffer for the side-chained in-process render: 0/1
    // are the main block, 2/3 the side-chain. Allocated once in the
    // constructor like childScratch_, never on the audio thread.
    juce::AudioBuffer<float> sideChainComposite_;
    std::atomic<int> sideChainChannels_{0};
    std::atomic<double> playSampleRate_{48000.0};
    std::atomic<int> playBufferSize_{256};
    std::atomic<int> childBlockDeadlineMs_{10};
    std::atomic<int> childConsecutiveMisses_{0};
    // Continuous time the slot has been missing blocks, in block durations. A
    // latch needs the run to span a second: three consecutive misses is 10-30 ms
    // of wall time and one scheduling hiccup (page fault, autosave, a plugin
    // state push) would otherwise bury the slot for the session.
    std::atomic<int> childMissRunMs_{0};
    std::atomic<bool> childFailed_{false};
    // Blocks to skip before probing a failed child again. A latched slot renders
    // dry and waits this long between attempts, so a wedge costs one deadline
    // wait per second instead of one per block - and a hiccup is not terminal.
    std::atomic<int> childRetryCountdown_{0};

    // Continuous-miss budget before a slot is declared failed, in milliseconds
    // of block time (see noteChildMiss).
    static constexpr int kChildMissLatchMs = 1000;

    void noteChildMiss();

    // Full-state recovery for parameter traffic the child could not receive.
    // Coalesced: one pending push, one snapshot, one push per round. The
    // snapshot has to read this object (the editor instance's state, the child
    // host pointer), so it is taken on the message thread; only the blocking
    // control request runs on a detached worker, which is handed the child host
    // by shared_ptr and value copies of the snapshot and never touches `this`.
    // paramResyncAlive_ keeps a queued callback off a destroyed host.
    //
    // Ticket protocol (no second flag, so no lost wakeup): a request is one
    // bump of neededGen; `active` says who owes the round. A requester that
    // finds a round in flight never has to signal it directly - the round
    // compares neededGen against the generation it snapshotted when it ends and
    // books one more round when they differ. Leaving active set while that
    // rearm is queued, and clearing it before the closing recheck, is what
    // makes a preemption between the two impossible to lose a request to.
    struct ResyncState {
        std::atomic<std::uint64_t> neededGen{0};
        std::atomic<bool> active{false};
    };
    void scheduleParamResync();
    // The round body, message thread only: snapshot and hand the blocking push
    // to the worker. Called by scheduleParamResync and by the rearm, which is
    // why it re-reads the generation instead of taking one from its caller.
    void startParamResyncRound();
    juce::String editorStateAsBase64() const;
    std::shared_ptr<ResyncState> paramResyncState_{std::make_shared<ResyncState>()};
    std::shared_ptr<std::atomic<bool>> paramResyncAlive_{
        std::make_shared<std::atomic<bool>>(true)};

    juce::DocumentWindow* editorWindow = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHost)
};
