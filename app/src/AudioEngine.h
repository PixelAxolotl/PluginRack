#pragma once

#include <JuceHeader.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <pluginrack/EqEngine.h>
#include <pluginrack/Meters.h>
#include <pluginrack/Scene.h>

#include "PluginHost.h"
#include "DeviceStream.h"

class AudioEngine {
public:
    struct InsertSlot {
        std::uint32_t id = 0;
        juce::String pluginPath;
        juce::String pluginName;
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
        std::vector<std::unique_ptr<InsertSlot>> inserts;
        juce::AudioBuffer<float> buffer;
        // Six-band channel EQ, tapped before the inserts (pre-fader), mirroring
        // core MixerEngine. Bypassed by default to match scene v3 eqEnabled=false
        // and keep output bit-identical to the pre-EQ engine. eq/eqBands are
        // touched only under stateLock (render holds it; setters take it),
        // mirroring core's EqState mutex; eqBypassed/eqDirty are atomics so the
        // UI can read them without the lock.
        pluginrack::EqEngine eq;
        std::vector<pluginrack::EqBand> eqBands{pluginrack::defaultSixBands()};
        std::vector<pluginrack::DynamicsEffect> effects{pluginrack::defaultDynamicsEffects()};
        std::vector<pluginrack::DynamicsProcessor> effectProcessors;
        juce::Atomic<bool> eqBypassed{true};
        juce::Atomic<bool> eqDirty{false};
        AnalyzerTap analyzerTap; // UI-thread spectrum feed for the EQ window
        juce::String inputDeviceName;
        juce::String inputError;
        pluginrack::SourceKind sourceKind = pluginrack::SourceKind::Hardware;
        std::string sourceDeviceId;
        pluginrack::ChannelLayout layout = pluginrack::ChannelLayout::Stereo;
        std::vector<int> inputChannels{0, 1};
        // Shared per-device capture; several logical channels may map into it.
        std::shared_ptr<InputCapture> input;
    };

    struct Output {
        std::uint32_t id = 0;
        juce::String name;
        juce::Atomic<float> peak{0.0f};
        juce::Atomic<float> gainDb{0.0f};
        juce::Atomic<bool> muted{false};
        pluginrack::ChannelLayout layout = pluginrack::ChannelLayout::Stereo;
        juce::Atomic<bool> flipped{false};
        std::vector<std::unique_ptr<InsertSlot>> inserts;
        juce::AudioBuffer<float> buffer;
        // Output EQ, tapped before the output inserts, mirroring core's
        // pre-master-write output tap. Same locking rules as Channel.
        pluginrack::EqEngine eq;
        std::vector<pluginrack::EqBand> eqBands{pluginrack::defaultSixBands()};
        std::vector<pluginrack::DynamicsEffect> effects{pluginrack::defaultDynamicsEffects()};
        std::vector<pluginrack::DynamicsProcessor> effectProcessors;
        juce::Atomic<bool> eqBypassed{true};
        juce::Atomic<bool> eqDirty{false};
        AnalyzerTap analyzerTap; // UI-thread spectrum feed for the EQ window
        juce::String outputDeviceName;
        juce::String outputError;
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
        int inputBufferSize = 0;
        int outputBufferSize = 0;
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

    void prepareToPlay(int maxBlockFrames);
    void openDefaultDevices();
    void render(int frames);
    std::uint64_t getOversizedBlockCount() const { return oversizedBlockCount_.load(); }
    void releaseResources();
    // Ensures every channel with an assigned input device shares one capture
    // opened wide enough for the highest physical index it maps. Callers that
    // change inputChannels/layout must re-assign the device or call this.
    void prepareInputs();

    void addChannel();
    void removeChannel(std::uint32_t channelId);
    void addOutput();
    juce::String removeOutput(std::uint32_t outputId);    juce::String setChannelInputDevice(std::uint32_t channelId, const juce::String& deviceName);
    juce::String setOutputDevice(std::uint32_t outputId, const juce::String& deviceName);
    juce::String setDeviceBufferSize(int size);
    juce::String setInputBufferSize(int size);
    juce::String setOutputBufferSize(int size);
    bool openFullDuplexIfPossible(const juce::String& inputDevice, const juce::String& outputDevice);
    bool isFullDuplex() const { return fullDuplex_; }
    juce::String getClockDescription() const;
    juce::StringArray getAvailableInputDevices() const;
    juce::StringArray getAvailableOutputDevices() const;
    juce::String addPlugin(std::uint32_t channelId, const juce::File& file);
    void removePlugin(std::uint32_t channelId, std::uint32_t insertId);
    void setPluginBypassed(std::uint32_t channelId, std::uint32_t insertId, bool bypassed);
    juce::String addOutputPlugin(std::uint32_t outputId, const juce::File& file);
    struct HighestLatencyInsert { std::uint32_t channelId = 0; std::uint32_t insertId = 0; juce::String name; int latencySamples = 0; };
    HighestLatencyInsert getHighestLatencyInsert(std::uint32_t channelId) const;
    void removeOutputPlugin(std::uint32_t outputId, std::uint32_t insertId);
    void setOutputPluginBypassed(std::uint32_t outputId, std::uint32_t insertId, bool bypassed);

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
    // Outputs are indexed by scene-order position like core; channels by id.
    void setChannelEq(std::uint32_t channelId, const std::vector<pluginrack::EqBand>& bands);
    void setChannelEqBypassed(std::uint32_t channelId, bool bypassed);
    void setOutputEq(int outIdx, const std::vector<pluginrack::EqBand>& bands);
    void setOutputEqBypassed(int outIdx, bool bypassed);
    void setChannelEffects(std::uint32_t channelId,
                           const std::vector<pluginrack::DynamicsEffect>& effects);
    void setOutputEffects(std::uint32_t outputId,
                          const std::vector<pluginrack::DynamicsEffect>& effects);

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

    void startLatencyMeasurement();
    void cancelLatencyMeasurement();
    bool isLatencyMeasuring() const { return latencyMeasureActive_.load(std::memory_order_relaxed); }
    bool hasMeasuredLatency() const { return latencyMeasuredValid_.load(std::memory_order_relaxed); }
    bool didLastMeasurementFail() const { return latencyMeasureFailed_.load(std::memory_order_relaxed); }
    double getMeasuredLatencyMs() const;

    pluginrack::Scene makeScene() const;
    // Rebuilds strips, plugins, and devices from a saved scene. MUST run on
    // the JUCE message thread: plugin instantiation, editor-window
    // destruction with the old strips, and device open/close are message-
    // thread operations. The audio clock is parked around the strip swap
    // so the call stays brief. Returns plugin/device load errors, empty on
    // full success.
    juce::String restoreScene(const pluginrack::Scene& scene);

private:
    Channel* findChannel(std::uint32_t channelId);
    InsertSlot* findInsert(std::uint32_t channelId, std::uint32_t insertId);
    Output* findOutput(std::uint32_t outputId);
    InsertSlot* findOutputInsert(std::uint32_t outputId, std::uint32_t insertId);
    static float decibelsToGain(float gainDb);
    // Audio-thread silence feed: pushes a fade-to-silence block to every
    // published output. Never allocates and never takes stateLock, so it also
    // works on the render-miss path when the message thread holds the lock.
    void pushSilenceToOutputs(int frames);
    void pushSilenceToOutputsLocked(int frames);
    // Rebuilds the lock-free playback snapshot. Requires stateLock.
    void publishRenderOutputsLocked();
    // Drops captures no channel references anymore (closes their device).
    // Requires stateLock.
    void pruneUnusedCaptures();
    void cancelLatencyMeasurementImpl();

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

    // Round-trip latency measurement state.
    std::atomic<bool> latencyMeasureActive_{false};
    std::atomic<bool> latencyMeasureEmitted_{false};
    std::atomic<std::uint64_t> latencyMeasureOutputSample_{0};
    std::atomic<int> latencyMeasureBlocks_{0};
    std::atomic<bool> latencyMeasuredValid_{false};
    std::atomic<int> latencyMeasuredSamples_{0};
    std::atomic<std::uint32_t> latencyMeasureStartMs_{0};
    std::atomic<bool> latencyMeasureFailed_{false};
    std::uint64_t outputSampleCounter_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
