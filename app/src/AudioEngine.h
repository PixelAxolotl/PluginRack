#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pluginrack/AudioTelemetry.h>
#include <pluginrack/DeviceCache.h>
#include <pluginrack/EqEngine.h>
#include <pluginrack/LatencyProfile.h>
#include <pluginrack/Meters.h>
#include <pluginrack/Scene.h>
#include <pluginrack/MultibandEffect.h>
#include <pluginrack/DeEsserEffect.h>
#include <pluginrack/LimiterEffect.h>

#include "PluginHost.h"
#include "DeviceStream.h"

class AudioEngine {
public:
    // Global VST insert budget: every input and output strip shares this one
    // constant, so the UI's add-row and the engine's refusal can never drift
    // apart. The ceiling is high enough that normal sessions never hit it;
    // it exists so a runaway add-loop cannot allocate without bound.
    static constexpr int kMaxInserts = 64;

    struct InsertSlot {
        std::uint32_t id = 0;
        juce::String pluginPath;
        juce::String pluginName;
        // The VST3 class inside the bundle this slot hosts; 0 when unknown.
        int pluginUniqueId = 0;
        PluginHost pluginHost;
        juce::Atomic<bool> bypassed{false};
        int latencySamples = 0;
    };

    // Task 6 analyzer tap: when an EQ window turns the spectrum on, render()
    // memcpy-copies the block around its EQ call into these fixed rings; the
    // window's UI-thread timer snapshot-copies a ring and runs the FFT there.
    // Audio-thread work is mode-gated and allocation-free (rings are sized at
    // strip construction); analyzer math never touches the audio thread. The
    // UI read races the audio write by design (peak/meters precedent): a
    // torn sample only shifts one visual frame, so mode and cursors are
    // atomics and the rings are never resized.
    struct AnalyzerTap {
        static constexpr int kSamples = 8192; // == EQWindow::kAnalyzerFftSize
        static constexpr std::uint32_t kOff = 0;  // mirrors kAnalyzerOff
        static constexpr std::uint32_t kPre = 1;  // mirrors kAnalyzerPre
        static constexpr std::uint32_t kPost = 2; // mirrors kAnalyzerPost

        // JUCE leaves fresh AudioBuffer samples undefined; snapshot() can
        // read the rings before the first writeTap, so start them silent.
        AnalyzerTap() {
            pre.clear();
            post.clear();
        }

        void setMode(std::uint32_t newMode) noexcept {
            mode.set(newMode);
        }
        // Audio thread: copy the block into the ring of the tapped side.
        void writeTap(bool postEq, const float* inL, const float* inR, int frames) noexcept;
        // UI thread: copy the newest maxFrames samples (oldest first) of one
        // side into destL/destR; dest capacity is assumed to be maxFrames.
        void snapshot(bool postEq, float* destL, float* destR, int maxFrames) const noexcept;

        juce::AudioBuffer<float> pre{ 2, kSamples };
        juce::AudioBuffer<float> post{ 2, kSamples };
        juce::Atomic<std::uint32_t> mode{ kOff };
        juce::Atomic<std::uint32_t> preWrite{ 0 };  // next write index (ring cursor)
        juce::Atomic<std::uint32_t> postWrite{ 0 };
    };

    struct Channel {
        std::uint32_t id = 0;
        juce::String name;
        juce::Atomic<float> gainDb{0.0f};
        juce::Atomic<float> trimDb{0.0f};
        juce::Atomic<float> pan{0.0f};
        juce::Atomic<bool> muted{false};
        juce::Atomic<bool> soloed{false};
        juce::Atomic<float> peak{0.0f};
        // Per-side peaks for the stereo meter, written by render()
        // immediately after the fader and read by the UI without a lock, like
        // peak above.
        juce::Atomic<float> peakL{0.0f};
        juce::Atomic<float> peakR{0.0f};
        // Per-side RMS for the meter's solid bar, written by render() at the
        // same point as the peaks and read by the UI without a lock.
        juce::Atomic<float> rmsL{0.0f};
        juce::Atomic<float> rmsR{0.0f};
        std::vector<std::unique_ptr<InsertSlot>> inserts;
        std::vector<pluginrack::ChainItem> chain;
        juce::AudioBuffer<float> buffer;
        // Six-band channel EQ, tapped before the inserts (pre-fader), mirroring
        // core MixerEngine. Bypassed by default to match scene v3 eqEnabled=false
        // and keep output bit-identical to the pre-EQ engine. eq/eqBands are
        // touched only under stateLock (render holds it; setters take it),
        // mirroring core's EqState mutex; eqBypassed/eqDirty are atomics so the
        // UI can read them without the lock.
        pluginrack::EqEngine eq;
        std::vector<pluginrack::EqBand> eqBands{pluginrack::defaultEqBands()};
        // Post-EQ delay (0..EqEngine::kMaxDelayMs ms); message-thread state
        // like eqBands, mirrored into eq via setDelayMs under stateLock.
        float eqDelayMs = 0.0f;
        // Strip identity hue (degrees); 0 = unassigned, UI assigns on load.
        float colourHue = 0.0f;
        // EQ polarity flip; mirrored into eq via setPhaseFlip under stateLock.
        juce::Atomic<bool> eqPhaseFlip{false};
        // RTA display offset in dB (0 == the default view). Display only;
        // message-thread state saved with the scene like eqBands.
        float eqRtaOffsetDb = 0.0f;
        // Analyzer weighting: 0 = Z, 1 = A, 2 = C. Display only.
        int eqRtaWeighting = 0;
        float multibandRtaOffsetDb = 0.0f;
        int multibandRtaWeighting = 0;
        float deEsserRtaOffsetDb = 0.0f;
        int deEsserRtaWeighting = 0;
        std::vector<pluginrack::DynamicsEffect> effects{pluginrack::defaultDynamicsEffects()};
        std::vector<pluginrack::DynamicsProcessor> effectProcessors;
        std::vector<pluginrack::MultibandEffect> multibandEffects;
        std::vector<pluginrack::MultibandProcessor> multibandProcessors;
        std::vector<pluginrack::DeEsserEffect> deEsserEffects;
        std::vector<pluginrack::DeEsserProcessor> deEsserProcessors;
        std::vector<pluginrack::LimiterEffect> limiterEffects;
        std::vector<pluginrack::LimiterProcessor> limiterProcessors;
        juce::Atomic<bool> eqBypassed{true};
        juce::Atomic<bool> eqDirty{false};
        // One tap per built-in, each written at its own point in the chain:
        // eqTap after the EQ, dynamicsTap after Comp/Gate, multibandTap
        // after Multiband, deEsserTap after De-esser, limiterTap after
        // Limiter. That's what makes each RTA *independent* — the Multiband
        // RTA shows what leaves the Multiband, not what leaves the EQ
        // (before it in the chain) or the strip end. The shared
        // displayRtaLevelDb()/spectrumLevelAtDb() math stays shared; only
        // the data feeding it is per-plugin now. analyzerTap is kept as
        // the strip-end (post-everything) legacy feed for the mini cards.
        AnalyzerTap eqTap;
        AnalyzerTap dynamicsTap;
        AnalyzerTap multibandTap;
        AnalyzerTap deEsserTap;
        AnalyzerTap limiterTap;
        AnalyzerTap analyzerTap; // UI-thread spectrum feed for the EQ window
        // Guarded by stateLock; read them through getChannelDeviceState().
        juce::String inputDeviceName;
        juce::String inputError;
        pluginrack::SourceKind sourceKind = pluginrack::SourceKind::Hardware;
        std::string sourceDeviceId;
        pluginrack::ChannelLayout layout = pluginrack::ChannelLayout::Stereo;
        std::vector<int> inputChannels{0, 1};
        // Shared per-device capture; several logical channels may map into it.
        std::shared_ptr<InputCapture> input;
        // Fed by the clock's duplex callback instead of a capture: the same
        // driver client plays and records, so there is no ring to read.
        juce::Atomic<bool> duplexInput{false};
        // Physical inputs the duplex device exposes, for the input menus.
        int duplexInputChannels = 0;
    };

    struct Output {
        std::uint32_t id = 0;
        juce::String name;
        juce::Atomic<float> peak{0.0f};
        // Per-side peaks for the stereo meter; same writer/reader rule as the
        // channel strip's pair.
        juce::Atomic<float> peakL{0.0f};
        juce::Atomic<float> peakR{0.0f};
        juce::Atomic<float> rmsL{0.0f};
        juce::Atomic<float> rmsR{0.0f};
        juce::Atomic<float> gainDb{0.0f};
        juce::Atomic<bool> muted{false};
        pluginrack::ChannelLayout layout = pluginrack::ChannelLayout::Stereo;
        juce::Atomic<bool> flipped{false};
        std::vector<std::unique_ptr<InsertSlot>> inserts;
        std::vector<pluginrack::ChainItem> chain;
        juce::AudioBuffer<float> buffer;
        // Output EQ, tapped before the output inserts, mirroring core's
        // pre-master-write output tap. Same locking rules as Channel.
        pluginrack::EqEngine eq;
        std::vector<pluginrack::EqBand> eqBands{pluginrack::defaultEqBands()};
        float eqDelayMs = 0.0f; // post-EQ delay, same rules as Channel
        float colourHue = 0.0f; // strip identity hue, same rules as Channel
        juce::Atomic<bool> eqPhaseFlip{false}; // EQ polarity flip, same as Channel
        float eqRtaOffsetDb = 0.0f; // RTA display offset, same rules as Channel
        int eqRtaWeighting = 0;     // analyzer weighting, same rules as Channel
        float multibandRtaOffsetDb = 0.0f;
        int multibandRtaWeighting = 0;
        float deEsserRtaOffsetDb = 0.0f;
        int deEsserRtaWeighting = 0;
        std::vector<pluginrack::DynamicsEffect> effects{pluginrack::defaultDynamicsEffects()};
        std::vector<pluginrack::DynamicsProcessor> effectProcessors;
        std::vector<pluginrack::MultibandEffect> multibandEffects;
        std::vector<pluginrack::MultibandProcessor> multibandProcessors;
        std::vector<pluginrack::DeEsserEffect> deEsserEffects;
        std::vector<pluginrack::DeEsserProcessor> deEsserProcessors;
        std::vector<pluginrack::LimiterEffect> limiterEffects;
        std::vector<pluginrack::LimiterProcessor> limiterProcessors;
        juce::Atomic<bool> eqBypassed{true};
        juce::Atomic<bool> eqDirty{false};
        // One tap per built-in, same rule as Channel.
        AnalyzerTap eqTap;
        AnalyzerTap dynamicsTap;
        AnalyzerTap multibandTap;
        AnalyzerTap deEsserTap;
        AnalyzerTap limiterTap;
        AnalyzerTap analyzerTap; // UI-thread spectrum feed for the EQ window
        // Guarded by stateLock; read them through getOutputDeviceState().
        juce::String outputDeviceName;
        juce::String outputError;
        // Device type the open selected ("Windows Audio", "ASIO", ...),
        // copied here so diagnostics never touch the playback object.
        juce::String outputDeviceType;
        std::unique_ptr<OutputPlayback> output;
    };

    struct Route {
        std::uint32_t channelId = 0;
        std::uint32_t outputId = 0;
        juce::Atomic<float> sendGainDbL{0.0f};
        juce::Atomic<float> sendGainDbR{0.0f};
    };

    struct LatencyReport {
        double inputDeviceMs = 0.0;
        double outputDeviceMs = 0.0;
        double pluginMs = 0.0;
        double fifoMs = 0.0;
        double totalMs = 0.0;
        // Per-stage split. inputDeviceMs/outputDeviceMs keep their historic
        // meaning (device latency plus the FIFO/buffer the stream adds) so
        // existing callers do not change; these break that total apart.
        double inputDeviceLatencyMs = 0.0; // driver-reported capture latency only
        double inputFifoDelayMs = 0.0;     // capture read delay (the jitter buffer)
        double outputDeviceLatencyMs = 0.0; // driver-reported playback latency only
        double outputBufferMs = 0.0;        // one device buffer on the clock output
        double unexpectedMs = 0.0;          // measured - predicted; 0 when not measured
        int inputBufferSize = 0;
        int outputBufferSize = 0;
        // What the user asked for (0 = "Auto: whatever the driver defaults
        // to"). Comparing it with the granted size is the only way to tell a
        // big buffer the app requested from one the driver imposed.
        int requestedInputBufferSize = 0;
        int requestedOutputBufferSize = 0;
        juce::String clockDeviceType;
        int inputReadDelayFrames = 0;
        int clockFillFrames = 0;
        int clockTargetFillFrames = 0;
        double sampleRate = 48000.0;
        std::uint64_t underruns = 0;
        std::uint64_t overruns = 0;
        std::uint32_t maxInsertId = 0;
        juce::String maxInsertName;
        double maxInsertMs = 0.0;
        bool valid = false;
        bool approximate = true;
        double measuredMs = 0.0;
        bool measuredValid = false;
        bool measuredAttempted = false;
        bool measureFailed = false;
        // True when capture and playback are different devices. Duplex is then
        // impossible and the read delay is the cost of bridging two clocks.
        bool separateClocks = false;
    };

    struct FifoDiagnostics {
        std::uint64_t inputOverruns = 0;
        std::uint64_t inputUnderruns = 0;
        std::uint64_t inputDroppedFrames = 0;
        std::uint64_t outputUnderruns = 0;
        std::uint64_t outputDroppedFrames = 0;
        std::uint64_t outputLowWatermarks = 0;
        std::uint64_t renderMisses = 0;
        double inputOccupancy = 0.0;
        double outputOccupancy = 0.0;
    };

    AudioEngine();
    ~AudioEngine();

    void prepareToPlay(int maxBlockFrames);
    void openDefaultDevices();
    // Assigns the default output/input and opens them on the startup worker
    // instead of blocking the message thread (see openStripsDevicesAsync).
    void openDefaultDevicesAsync();
    // Opens every device named by the current strips on a background thread.
    // Startup uses this after restoreScene(..., false) so the window stays
    // responsive while slow WASAPI drivers are probed (a USB endpoint can take
    // over ten seconds to enumerate its formats). All output streams open
    // concurrently against one snapshot of the engine rate (the first output
    // by id owns the clock role); the input captures follow once the clock's
    // final rate is known. Calls while a run is active are ignored. The
    // message thread consumes the result through takeDeviceOpenErrors() /
    // consumeDeviceOpenFinished().
    void openStripsDevicesAsync();
    bool isOpeningDevices() const { return openingDevices_.load(std::memory_order_relaxed); }
    int getDeviceOpenProgress() const { return deviceOpenProgress_.load(std::memory_order_relaxed); }
    int getDeviceOpenTotal() const { return deviceOpenTotal_.load(std::memory_order_relaxed); }
    // The endpoints being opened right now, e.g. `... input "Ch 1" - USB Audio`.
    // Every endpoint starts at once, so this renders the in-flight set rather
    // than one name: a single "current" string only ever showed the last
    // worker to start, which read as if devices were opened one at a time.
    juce::String getDeviceOpenStatus() const;
    // True exactly once after a background open finishes.
    bool consumeDeviceOpenFinished() {
        return deviceOpenFinished_.exchange(false, std::memory_order_acq_rel);
    }
    // Returns and clears the errors reported by the last background open.
    juce::String takeDeviceOpenErrors();

    // Latency/robustness trade-offs exposed in Settings. Changing one
    // reopens the affected streams; output changes run on the same background
    // opener as startup so the window stays responsive.
    enum class LatencyOption {
        LowLatencyInput,    // 1-block capture read delay (default 2)
        PreferExclusive,    // try exclusive WASAPI before the shared modes
        LowLatencyOutputs,  // 2-block FIFO target for non-clock outputs
        MinimalClockFifo,   // clock FIFO: no priming, silence top-up only
        FullDuplexInput     // same-device input: start with one-block delay
    };
    void setLatencyOption(LatencyOption option, bool enabled);
    bool getLatencyOption(LatencyOption option) const;

    // Everything the Settings page can change. Applied as one unit so a
    // device that refuses the new configuration can fall back cleanly.
    struct DeviceSettings {
        int inputBuffer = 0;
        int outputBuffer = 0;
        // Beta latency modes are ignored unless the user explicitly opts in
        // through Settings and applies the choice. Persisted with scenes so
        // an old session cannot silently turn them back on.
        bool latencyTuningOptIn = false;
        bool lowLatencyInput = false;
        bool preferExclusive = false;
        bool lowLatencyOutputs = false;
        bool minimalClockFifo = false;
        bool fullDuplexInput = false;
        // Not a stream option: kept here so it saves and loads with the rest.
        bool autoReloadDevices = false;
        // Scene v6 audio-pipeline hardening, off until the user opts in.
        bool slicedRender = false;
        bool nonFiniteScrub = false;
        bool healthReload = false;
        bool ladderRecovery = false;
        double healthYellowUnderrunsPerMinute = 1.0;
        double healthRedUnderrunsPerMinute = 10.0;
        double healthPinnedToEdgeMs = 5000.0;
        // Rack insert thumbnail refresh rate (fps); also not a stream option.
        int rackThumbnailFps = 15;
        // Which inserts keep live previews; also not a stream option.
        pluginrack::RackPreviewMode rackPreviewMode = pluginrack::RackPreviewMode::Selected;
    };
    DeviceSettings getDeviceSettings() const;
    // Reopens every device with these settings on the background opener. If a
    // device refuses to open, the previous settings are restored and the
    // previous streams are kept (they are only replaced on success).
    void applyDeviceSettings(const DeviceSettings& settings);
    // Reopens every device with the current settings (no setting change).
    void reloadAllDevicesAsync();

    // Auto-reload: the watcher lives in Settings, but the flag and the watched
    // strips are session state, so they belong to the engine that saves them.
    void setAutoReloadDevices(bool enabled);
    bool getAutoReloadDevices() const { return autoReloadDevices_.load(); }
    // Rack insert thumbnails: takes effect on the next UI tick, never reopens
    // a stream, so it is set straight from Settings instead of being staged.
    void setRackThumbnailFps(int fps);
    int getRackThumbnailFps() const { return rackThumbnailFps_.load(); }
    void setRackPreviewMode(pluginrack::RackPreviewMode mode);
    pluginrack::RackPreviewMode getRackPreviewMode() const { return rackPreviewMode_; }
    void setWatchedDevices(const std::vector<pluginrack::WatchedDevice>& devices);
    std::vector<pluginrack::WatchedDevice> getWatchedDevices() const;

    // Snapshot of a strip's device strings, copied under stateLock. The device
    // opener writes those fields on its own worker thread; copying a juce::String
    // that another thread is reassigning reads a torn StringHolder pointer and
    // crashes, so views must never read the strip fields directly.
    struct DeviceState {
        juce::String deviceName;
        juce::String error;
        // Physical inputs the assigned device exposes; 0 when none is open and
        // always 0 for outputs. Read under the same lock so the input menus can
        // size themselves without touching the capture object.
        int availableInputs = 0;
        // Backend the stream actually opened in ("ASIO", "Windows Audio", ...).
        // The strips show this so the user never has to guess.
        juce::String deviceType;
    };
    DeviceState getChannelDeviceState(std::uint32_t channelId) const;
    DeviceState getOutputDeviceState(std::uint32_t outputId) const;
    // Locked copies for the views that list failures or watch devices.
    std::vector<std::pair<juce::String, juce::String>> getDeviceErrors() const;
    std::vector<juce::String> getAssignedInputDeviceNames() const;

    struct ReloadableDevice {
        juce::String label;
        juce::String deviceName;
        std::uint32_t stripId = 0;
        bool isInput = false;
    };
    std::vector<ReloadableDevice> getReloadableDevices() const;
    // Reopens one device on the background opener. If it refuses, the current
    // stream is kept (the old playback/capture is only replaced on success).
    void reloadDeviceAsync(const ReloadableDevice& device);
    // Reopens several devices and automatically retries any that fail, so a
    // device that was busy during a reload gets another chance.
    void reloadDevicesAsync(const std::vector<ReloadableDevice>& devices);
    void render(int frames);
    std::uint64_t getOversizedBlockCount() const { return oversizedBlockCount_.load(); }
    // Deadline-aware rendering: blocks whose tail had to be silenced, and
    // non-finite samples quarantined at insert boundaries. Both are lifetime
    // counters; the rolling telemetry window carries the same events.
    std::uint64_t getPartialRenderCount() const { return partialRenders_.load(); }
    std::uint64_t getScrubbedSamples() const { return scrubbedSamples_.load(); }
    // Advisory per-endpoint capability cache (diagnostics / Settings). The
    // cache is persisted after every successful open; a stale entry is never
    // used, the live probe always wins.
    juce::String getDeviceCacheJson() const;
    void releaseResources();
    // Ensures every channel with an assigned input device shares one capture
    // opened wide enough for the highest physical index it maps. Callers that
    // change inputChannels/layout must re-assign the device or call this.
    void prepareInputs();

    void addChannel();
    // Removes a channel and its routes. Returns an empty string on success, or
    // the reason the change was refused: the startup opener holds Channel*
    // across a device probe, and the scene validator needs at least one
    // channel, so the last strip cannot be removed.
    juce::String removeChannel(std::uint32_t channelId);
    // Moves a channel to scene-order position targetIndex (clamped). The mixer's
    // drag handle calls this; the engine order is what scenes save, so the new
    // arrangement survives a save/load. Channel objects never move in memory
    // (only the owning unique_ptrs do), so strips' Channel& stays valid.
    void moveChannel(std::uint32_t channelId, int targetIndex);
    // Output-side twin of moveChannel: the mixer's output strips reorder with
    // the same handle. Routes and EQ windows address outputs by id/scene-order,
    // so a move only reshuffles the scene order.
    void moveOutput(std::uint32_t outputId, int targetIndex);
    void addOutput();
    juce::String removeOutput(std::uint32_t outputId);    juce::String setChannelInputDevice(std::uint32_t channelId, const juce::String& deviceName);
    juce::String setOutputDevice(std::uint32_t outputId, const juce::String& deviceName);
    juce::String setDeviceBufferSize(int size);
    juce::String setInputBufferSize(int size);
    juce::String setOutputBufferSize(int size);
    bool openFullDuplexIfPossible(const juce::String& inputDevice, const juce::String& outputDevice);
    bool isFullDuplex() const { return fullDuplex_; }
    juce::String getClockDescription() const;
    // The device name behind the clock output; empty when no output owns the
    // clock. I/O uses it to name the stream the measurement travels through.
    juce::String getClockOutputDeviceName() const;
    juce::StringArray getAvailableInputDevices() const;
    juce::StringArray getAvailableOutputDevices() const;
    // ASIO drivers this build can reach; empty means "Force ASIO" is a no-op.
    juce::StringArray getAsioDeviceNames() const;
    // Points the clock output at the ASIO device that corresponds to the
    // current clock endpoint, enabling the duplex switch so that interface can
    // also carry a capture. Returns a user-facing status line: what happened,
    // or what the user has to choose instead of guessing.
    juce::String forceAsio();
    // Moves input strips that name the same interface as `clockDevice` onto
    // that device, so the clock's duplex callback can feed them. Returns how
    // many strips moved. `previous` receives their old assignments so a failed
    // clock open can put them back.
    int retargetInputsToClockDevice(
        const juce::String& clockDevice,
        std::vector<std::pair<std::uint32_t, juce::String>>* previous = nullptr);
    // Message-thread watchdog: flags a strip whose assigned device is no longer
    // enumerated (unplugged or disabled while the stream was open), so the UI
    // shows it as missing instead of leaving the status dot green. Only adds
    // the marker; a successful open clears it. Returns true when any strip
    // changed. Cheap enough for a UI timer: enumeration is cached.
    bool refreshDeviceAvailability();
    // Loads the VST3 on a worker and installs it on the message thread:
    // instantiation blocks for as long as the plugin needs to initialise, and
    // doing that here froze the window. pluginUniqueId selects the class
    // inside a multi-class bundle (0 = the bundle's first one). done() runs on
    // the message thread with an error message, or an empty string on success.
    void addPluginAsync(std::uint32_t channelId, const juce::File& file, int pluginUniqueId,
                        std::function<void(juce::String)> done);
    void removePlugin(std::uint32_t channelId, std::uint32_t insertId);
    void setPluginBypassed(std::uint32_t channelId, std::uint32_t insertId, bool bypassed);
    void moveChannelChainItem(std::uint32_t channelId, pluginrack::ChainItem item, int targetIndex);
    // Removes a chain entry (a built-in effect). Picking the same effect from
    // the plugin menu appends it back; the scene save keeps whatever chain is
    // current, so a saved scene restores the removed state as-is.
    void removeChannelChainItem(std::uint32_t channelId, pluginrack::ChainItem item);
    void removeOutputChainItem(std::uint32_t outputId, pluginrack::ChainItem item);
    // Replaces a strip's chain order wholesale: "Reset to Defaults" needs to
    // put the built-in rows back, which no move can express.
    void setChannelChain(std::uint32_t channelId, const std::vector<pluginrack::ChainItem>& chain);
    void setOutputChain(std::uint32_t outputId, const std::vector<pluginrack::ChainItem>& chain);
    // The chain a freshly created or reset strip starts with.
    static std::vector<pluginrack::ChainItem> defaultChain();
    void addOutputPluginAsync(std::uint32_t outputId, const juce::File& file, int pluginUniqueId,
                              std::function<void(juce::String)> done);
    struct HighestLatencyInsert { std::uint32_t channelId = 0; std::uint32_t insertId = 0; juce::String name; int latencySamples = 0; };
    HighestLatencyInsert getHighestLatencyInsert(std::uint32_t channelId) const;
    void removeOutputPlugin(std::uint32_t outputId, std::uint32_t insertId);
    void setOutputPluginBypassed(std::uint32_t outputId, std::uint32_t insertId, bool bypassed);
    void moveOutputChainItem(std::uint32_t outputId, pluginrack::ChainItem item, int targetIndex);
    // Appends a built-in effect not currently in the strip's chain, so picking
    // a removed built-in from the plugin menu restores it. No-op if present.
    void ensureChannelChainItem(std::uint32_t channelId, pluginrack::ChainItem item);
    void ensureOutputChainItem(std::uint32_t outputId, pluginrack::ChainItem item);

    void setChannelGain(std::uint32_t channelId, float gainDb);
    void setChannelTrim(std::uint32_t channelId, float trimDb);
    void setChannelPan(std::uint32_t channelId, float pan);
    void setChannelName(std::uint32_t channelId, const juce::String& name);
    void setChannelMuted(std::uint32_t channelId, bool muted);
    void setChannelSoloed(std::uint32_t channelId, bool soloed);
    void setChannelLayout(std::uint32_t channelId, pluginrack::ChannelLayout layout);
    void setChannelInputChannels(std::uint32_t channelId, const std::vector<int>& inputChannels);
    void setChannelMapping(std::uint32_t channelId, pluginrack::ChannelLayout layout, const std::vector<int>& inputChannels);
    void setSendGain(std::uint32_t channelId, std::uint32_t outputId, float gainDb);
    void setSendGainL(std::uint32_t channelId, std::uint32_t outputId, float gainDb);
    void setSendGainR(std::uint32_t channelId, std::uint32_t outputId, float gainDb);
    float getSendGain(std::uint32_t channelId, std::uint32_t outputId) const;
    float getSendGainL(std::uint32_t channelId, std::uint32_t outputId) const;
    float getSendGainR(std::uint32_t channelId, std::uint32_t outputId) const;
    void setOutputGain(std::uint32_t outputId, float gainDb);
    void setOutputMuted(std::uint32_t outputId, bool muted);
    void setOutputName(std::uint32_t outputId, const juce::String& name);
    void setOutputLayout(std::uint32_t outputId, pluginrack::ChannelLayout layout);
    void setOutputFlipped(std::uint32_t outputId, bool flipped);

    // EQ API mirroring core MixerEngine: setChannelEq/setOutputEq swap bands
    // (bypass state kept) and rebuild coefficients on the message thread
    // under stateLock; the bypass setters take effect live, no re-prepare,
    // and reset filter state when engaging so re-enabling starts clean.
    // The delay setters size the line on first use and glide the read
    // distance, so they are equally safe to call while render() runs.
    // Outputs are indexed by scene-order position like core; channels by id.
    void setChannelEq(std::uint32_t channelId, const std::vector<pluginrack::EqBand>& bands);
    void setChannelEqBypassed(std::uint32_t channelId, bool bypassed);
    void setChannelEqDelay(std::uint32_t channelId, float delayMs);
    void setChannelEqPhaseFlip(std::uint32_t channelId, bool flipped);
    void setChannelColourHue(std::uint32_t channelId, float hueDegrees);
    void setChannelEqRtaOffset(std::uint32_t channelId, float offsetDb);
    void setChannelEqRtaWeighting(std::uint32_t channelId, int weighting);
    void setChannelMultibandRtaOffset(std::uint32_t channelId, float offsetDb);
    void setChannelMultibandRtaWeighting(std::uint32_t channelId, int weighting);
    void setChannelDeEsserRtaOffset(std::uint32_t channelId, float offsetDb);
    void setChannelDeEsserRtaWeighting(std::uint32_t channelId, int weighting);
    void setOutputEq(int outIdx, const std::vector<pluginrack::EqBand>& bands);
    void setOutputEqBypassed(int outIdx, bool bypassed);
    void setOutputEqDelay(int outIdx, float delayMs);
    void setOutputEqPhaseFlip(int outIdx, bool flipped);
    void setOutputColourHue(int outIdx, float hueDegrees);
    void setOutputEqRtaOffset(int outIdx, float offsetDb);
    void setOutputEqRtaWeighting(int outIdx, int weighting);
    void setOutputMultibandRtaOffset(int outIdx, float offsetDb);
    void setOutputMultibandRtaWeighting(int outIdx, int weighting);
    void setOutputDeEsserRtaOffset(int outIdx, float offsetDb);
    void setOutputDeEsserRtaWeighting(int outIdx, int weighting);
    void setChannelEffects(std::uint32_t channelId,
                           const std::vector<pluginrack::DynamicsEffect>& effects);
    void setOutputEffects(std::uint32_t outputId,
                          const std::vector<pluginrack::DynamicsEffect>& effects);
    void setChannelMultiband(std::uint32_t channelId,
                             const std::vector<pluginrack::MultibandEffect>& effects);
    void setOutputMultiband(std::uint32_t outputId,
                            const std::vector<pluginrack::MultibandEffect>& effects);
    void setChannelDeEssers(std::uint32_t channelId,
                            const std::vector<pluginrack::DeEsserEffect>& effects);
    void setOutputDeEssers(std::uint32_t outputId,
                           const std::vector<pluginrack::DeEsserEffect>& effects);
    void setChannelLimiters(std::uint32_t channelId,
                            const std::vector<pluginrack::LimiterEffect>& effects);
    void setOutputLimiters(std::uint32_t outputId,
                           const std::vector<pluginrack::LimiterEffect>& effects);

    const std::vector<std::unique_ptr<Channel>>& getChannels() const;
    const std::vector<std::unique_ptr<Output>>& getOutputs() const;
    const std::vector<std::unique_ptr<Route>>& getRoutes() const;

    Channel* getChannelById(std::uint32_t channelId) {
        for (auto& c : channels) {
            if (c->id == channelId) return c.get();
        }
        return nullptr;
    }

    Output* getOutputById(std::uint32_t outputId) {
        for (auto& o : outputs) {
            if (o->id == outputId) return o.get();
        }
        return nullptr;
    }

    LatencyReport getLatencyReport() const;

    FifoDiagnostics getFifoDiagnostics() const;
    // Green/yellow/red per stream, from the same underrun + fill window the
    // strips display. A missing stream reads green so the auto-reload
    // controller falls back to the error path.
    pluginrack::StreamHealth getChannelHealth(std::uint32_t channelId) const;
    pluginrack::StreamHealth getOutputHealth(std::uint32_t outputId) const;
    bool getSlicedRenderEnabled() const { return slicedRender_.load(); }
    bool getNonFiniteScrubEnabled() const { return nonFiniteScrub_.load(); }
    bool getHealthReloadDevices() const { return healthReload_.load(); }
    bool getLadderRecoveryEnabled() const { return ladderRecovery_.load(); }
    pluginrack::StreamHealthThresholds getHealthThresholds() const;
    // Render-load summary from the clock callback: percentiles of the time
    // spent inside render(), the share of a block period that p99 costs, and
    // the fill distribution that shows whether the FIFO target is right.
    pluginrack::AudioTelemetry::Summary getRenderTelemetry() const;

    struct LatencyProfileState {
        bool active = false;
        bool finished = false;
        bool failed = false;
        int stepIndex = 0;
        int totalSteps = 0;
        int startBuffer = 0;
        int currentBuffer = 0;
        int remainingMs = 0;
    };
    // Guided search for the smallest buffer this device survives: it steps the
    // size down one notch, watches for real audio events, and reverts to the
    // starting size on the first failure. Every step goes through
    // applyDeviceSettings, so a refused size still restores the previous one.
    void startLatencyProfile();
    void cancelLatencyProfile();
    // Call from a UI timer while the search is active.
    LatencyProfileState tickLatencyProfile();
    LatencyProfileState getLatencyProfileState() const;
    bool isLatencyProfileActive() const { return latencyProfile_.status().active; }

    // listenDevice selects the capture that listens for the returning click
    // (an open input device name). Empty means any open input.
    void startLatencyMeasurement(const juce::String& listenDevice = {});
    void cancelLatencyMeasurement();
    // Message-thread watchdog for a measurement that can never finish: no
    // clock output renders, no input capture is open, or the audio clock is
    // stopped. The render path times out a hunt it started; this covers the
    // case where nothing renders at all. Safe to call at UI refresh rate.
    void pollLatencyMeasurement();
    // Drops a stale result; called whenever the device graph or its buffer
    // configuration changes, because the old figure no longer describes it.
    void resetLatencyMeasurement();
    bool isLatencyMeasuring() const { return latencyMeasureActive_.load(std::memory_order_relaxed); }
    bool hasMeasuredLatency() const { return latencyMeasuredValid_.load(std::memory_order_relaxed); }
    bool didLastMeasurementFail() const { return latencyMeasureFailed_.load(std::memory_order_relaxed); }
    double getMeasuredLatencyMs() const;

    pluginrack::Scene makeScene() const;
    // Rebuilds strips, plugins, and devices from a saved scene. MUST run on
    // the JUCE message thread: plugin instantiation, editor-window
    // destruction with the old strips, and strip swapping are message-thread
    // operations. openDevicesNow=false records the scene's device assignments
    // and leaves the streams closed; the caller then runs
    // openStripsDevicesAsync() so a slow driver probe never blocks the UI.
    // Returns plugin/device load errors, empty on full success.
    juce::String restoreScene(const pluginrack::Scene& scene, bool openDevicesNow = true);

private:
    class DeviceOpenThread;
    class InternalClock;

    Channel* findChannel(std::uint32_t channelId);
    InsertSlot* findInsert(std::uint32_t channelId, std::uint32_t insertId);
    Output* findOutput(std::uint32_t outputId);
    InsertSlot* findOutputInsert(std::uint32_t outputId, std::uint32_t insertId);
    static float decibelsToGain(float gainDb);
    // Implementation of the public device setters without the "starting up"
    // check: the background opener calls these directly from its worker.
    juce::String setChannelInputDeviceImpl(std::uint32_t channelId, const juce::String& deviceName,
                                           bool forceReopen = false,
                                           double engineRate = 0.0);
    // engineRate lets the startup opener open every output against one
    // snapshot of the clock rate instead of racing the clock's own open.
    juce::String setOutputDeviceImpl(std::uint32_t outputId, const juce::String& deviceName,
                                     double engineRate);
    void prepareToPlayImpl(int maxBlockFrames, bool openInputs);
    // One full open pass over every output and input; returns the errors
    // without publishing them, so the caller can retry with previous settings.
    juce::String openAllDevicesPass(const std::function<bool()>& shouldExit, bool forceInputs);
    void openStripsDevicesNow(const std::function<bool()>& shouldExit, bool forceInputs);
    void startDeviceJob(std::function<void(const std::function<bool()>&)> job);
    // Tracks which endpoints are mid-open so the UI can show them all.
    void beginDeviceOpen(const juce::String& label);
    void endDeviceOpen(const juce::String& label);
    void clearDeviceOpens();
    void applySettingsValues(const DeviceSettings& settings);
    void stopDeviceOpenThread();
    // Drops separate captures on the clock device before the clock reopens:
    // ASIO is single-client, and an input holding the endpoint blocks the clock.
    void releaseDeviceInputsForClock(const juce::String& deviceName);
    // Audio-thread silence feed: pushes a fade-to-silence block to every
    // published output. Never allocates and never takes stateLock, so it also
    // works on the render-miss path when the message thread holds the lock.
    void pushSilenceToOutputs(int frames);
    void pushSilenceToOutputsLocked(int frames);
    // Audio thread: copies one capture window out of the duplex device buffers
    // into a channel, applying its physical-channel mapping. Same rules as
    // InputCapture::pull (mono maps one channel, the rest stay silent).
    void pullDuplexInput(Channel& channel, int frames) const;
    // Audio-thread telemetry: one record per clock callback. Never allocates
    // or blocks, so it can be called on every exit path of render().
    void recordRenderTelemetry(int frames, std::uint32_t renderNanos, std::uint16_t flags,
                               std::int32_t outputFill) const;
    // Rebuilds the lock-free playback snapshot. Requires stateLock.
    void publishRenderOutputsLocked();
    // Drops captures no channel references anymore (closes their device).
    // Requires stateLock.
    void pruneUnusedCaptures();
    void cancelLatencyMeasurementImpl();
    // Clock failover: promote the next live output in the same rate class, or
    // keep rendering on a free-running internal clock when none qualifies.
    // Called from the message-thread availability watchdog; never silent.
    void handleClockLoss();
    void startInternalClock(int sampleRate);
    void stopInternalClock();
    // Records and persists what one endpoint cost and negotiated. Worker
    // thread; serialized by deviceCacheLock_. A failure to write is logged,
    // never fatal: the cache is advisory.
    void recordDeviceCapabilities(const juce::String& endpoint, bool input,
                                  int inputChannels, int outputChannels,
                                  double sampleRate, int bufferSize, int probeMs);
    // Fresh cache entry only; a stale one is ignored and the live probe runs.
    std::optional<pluginrack::EndpointCapabilities> cachedCapabilities(
        const juce::String& endpoint) const;
    // Pushes the current health thresholds into every open stream tracker.
    void applyHealthThresholdsToStreams();

    struct SharedInputCapture {
        std::shared_ptr<InputCapture> capture;
        // Physical input count the device was opened with; >= a later request
        // means the capture can be reused even when the device itself offers
        // fewer inputs (missing ones stay silent and are reported).
        int requestedChannels = 0;
        // Buffer size the capture was opened with. Drivers may clamp to the
        // nearest supported size, so reuse compares requests, not actuals.
        int requestedBufferSize = 0;
        // Render block the ring was sized for; growing past it needs a reopen.
        int renderBlockFrames = 0;
    };

    // Removed plugins stay alive until the audio engine is destroyed. Some
    // VSTs keep worker callbacks active after removal, so destroying them on
    // the UI thread during live playback is unsafe.
    std::vector<std::unique_ptr<InsertSlot>> retiredInserts_;
    std::vector<std::unique_ptr<Channel>> channels;
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<std::unique_ptr<Route>> routes;
    std::map<juce::String, SharedInputCapture> inputCaptures_;
    pluginrack::MeterBank meters;
    mutable juce::CriticalSection stateLock;
    std::uint32_t nextChannelId = 1;
    std::uint32_t nextOutputId = 1;
    std::uint32_t nextInsertId = 1;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 256;
    int requestedBufferSize_ = 0;
    int requestedInputBufferSize_ = 0;
    int requestedOutputBufferSize_ = 0;
    int maxBlockFrames_ = 256;
    std::atomic<std::uint64_t> oversizedBlockCount_{0};
    std::atomic<std::uint64_t> renderMisses_{0};
    std::atomic<std::uint64_t> partialRenders_{0};
    std::atomic<std::uint64_t> scrubbedSamples_{0};
    // Written by the clock callback only, read by the UI. Mutable so the
    // const report getters can summarise it without a lock.
    mutable pluginrack::AudioTelemetry renderTelemetry_;

    // Guided buffer search state. Message thread only.
    pluginrack::LatencyProfile latencyProfile_;
    std::uint32_t latencyProfileLastMs_ = 0;
    std::uint64_t latencyProfileLastEvents_ = 0;
    bool latencyProfileAwaitingOpen_ = false;
    int latencyProfileRequested_ = 0;
    // Lock-free playback snapshot for the render-miss silence path. Slots are
    // published under stateLock and never freed until the engine is destroyed
    // (retiredOutputs_), so the audio thread can load a pointer and push to it
    // without racing an output swap. Closed playbacks ignore the push.
    static constexpr std::size_t kMaxRenderOutputs = 64;
    std::array<std::atomic<OutputPlayback*>, kMaxRenderOutputs> renderOutputs_{};
    std::vector<std::unique_ptr<OutputPlayback>> retiredOutputs_;
    // Incremented once per rendered block; channels sharing an InputCapture
    // use it to read one window per block instead of consuming it per channel.
    std::uint64_t renderEpoch_ = 0;
    std::uint32_t clockOutputId = 0;
    bool fullDuplex_ = false;
    std::unique_ptr<InternalClock> internalClock_;
    bool clockFailoverActive_ = false;
    mutable juce::CriticalSection deviceCacheLock_;
    pluginrack::DeviceCache deviceCache_;

    // Background device opener (startup path). Kept last so its lifetime ends
    // before the strips it reports on.
    std::unique_ptr<DeviceOpenThread> deviceOpenThread_;
    // Cleared in the destructor so a prepare callback posted from the device
    // worker cannot run against a destroyed engine.
    std::shared_ptr<std::atomic<bool>> prepareAliveToken_ =
        std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> openingDevices_{false};
    std::atomic<int> deviceOpenProgress_{0};
    std::atomic<int> deviceOpenTotal_{0};
    std::atomic<bool> deviceOpenFinished_{false};
    juce::CriticalSection deviceOpenErrorsLock_;
    juce::String deviceOpenErrors_;
    // Endpoints mid-open. Each open runs on its own thread with its own COM
    // apartment, so several are in flight at once; this lock guards the small
    // label list the UI renders.
    mutable juce::CriticalSection deviceOpenActiveLock_;
    std::vector<juce::String> deviceOpenActive_;

    // v5 graduation: a fresh session starts with the two bounded low-latency
    // switches on. Both self-correct (a starvation grows the read delay / the
    // FIFO target and a long calm period gives it back), which is what makes
    // them safe to default on. Exclusive mode and the minimal clock FIFO stay
    // opt-in because they cannot heal themselves.
    std::atomic<bool> latencyTuningOptIn_{true};
    std::atomic<bool> lowLatencyInput_{true};
    std::atomic<bool> preferExclusive_{false};
    std::atomic<bool> lowLatencyOutputs_{true};
    std::atomic<bool> minimalClockFifo_{false};
    std::atomic<bool> fullDuplexInput_{false};
    // Same-device duplex: the clock callback carries the captures too. The name
    // is guarded by stateLock; the pointers are only touched on the audio
    // thread inside that callback.
    std::atomic<bool> duplexActive_{false};
    juce::String duplexDeviceName_;
    const float* const* duplexInputBuffers_ = nullptr;
    int duplexInputBufferCount_ = 0;
    std::atomic<bool> autoReloadDevices_{false};
    std::atomic<bool> slicedRender_{false};
    std::atomic<bool> nonFiniteScrub_{false};
    std::atomic<bool> healthReload_{false};
    std::atomic<bool> ladderRecovery_{false};
    std::atomic<double> healthYellowUnderrunsPerMinute_{1.0};
    std::atomic<double> healthRedUnderrunsPerMinute_{10.0};
    std::atomic<double> healthPinnedToEdgeMs_{5000.0};
    std::atomic<int> rackThumbnailFps_{15};
    // Plain enum: only ever touched on the message thread (Settings writes,
    // RackView reads), so no atomic needed.
    pluginrack::RackPreviewMode rackPreviewMode_ = pluginrack::RackPreviewMode::Selected;
    juce::CriticalSection watchedDevicesLock_;
    std::vector<pluginrack::WatchedDevice> watchedDevices_;

    // Round-trip latency measurement state.
    std::atomic<bool> latencyMeasureActive_{false};
    std::atomic<bool> latencyMeasureEmitted_{false};
    std::atomic<bool> latencyMeasuredValid_{false};
    std::atomic<int> latencyMeasuredSamples_{0};
    std::atomic<double> latencyMeasuredRate_{0.0};
    std::atomic<std::uint32_t> latencyMeasureStartMs_{0};
    std::atomic<bool> latencyMeasureFailed_{false};
    // Which capture listens for the return. Written by startLatencyMeasurement
    // under stateLock and read by render(), which also holds it.
    juce::String latencyListenDevice_;
    std::uint64_t outputSampleCounter_ = 0;

    void resetLatencyMeasurementLocked();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
