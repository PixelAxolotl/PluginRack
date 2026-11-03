#pragma once

#include <JuceHeader.h>

#include <pluginrack/CaptureCursor.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace DeviceEnumeration {
juce::StringArray inputs();
juce::StringArray outputs();
}

class InputCapture : public juce::AudioIODeviceCallback {
public:
    InputCapture();
    ~InputCapture() override;

    // numChannelsNeeded = highest selected physical input index + 1 across the
    // logical channels using this device. At least 2 inputs are requested so
    // stereo passthrough keeps working. Inputs the device does not provide
    // stay silent and are reported by getInputError().
    juce::String open(const juce::String& preferredDeviceName,
                      double engineSampleRate,
                      int numChannelsNeeded,
                      juce::String& errorOut,
                      int requestedBufferSize = 0,
                      int renderBlockFrames = 0,
                      // Read delay in render blocks: 2 absorbs callback phase
                      // jitter, 1 is the low-latency choice.
                      int readDelayBlocks = 2,
                      // Try exclusive mode before the shared modes.
                      bool preferExclusive = false,
                      // Extra ring history for a render block the clock may
                      // still be negotiating. Sizes the ring only: it must not
                      // reach the read delay, which stays tied to the real
                      // render block or it turns straight into input latency.
                      int capacityReserveFrames = 0);
    juce::String open(const juce::String& preferredDeviceName,
                      double engineSampleRate,
                      juce::String& errorOut,
                      int requestedBufferSize = 0)
    {
        return open(preferredDeviceName, engineSampleRate, 2, errorOut, requestedBufferSize);
    }
    // Stereo passthrough for callers that do not select physical channels
    // yet (until AudioEngine passes per-channel mappings).
    juce::String open(const juce::String& preferredDeviceName,
                      double engineSampleRate,
                      juce::String& errorOut)
    {
        return open(preferredDeviceName, engineSampleRate, 2, errorOut);
    }
    void close();

    double getSampleRate() const { return sampleRate; }
    juce::String getError() const { return error; }
    // Device type the last successful open() selected (e.g. "Windows Audio
    // (Low Latency Mode)"). Diagnostics only; empty while closed.
    juce::String getActiveDeviceType() const { return activeDeviceType_; }

    int inputChannelCount() const { return inputChannelCount_; }
    // Total inputs the device itself provides; may exceed inputChannelCount(),
    // which is capped to what the opener requested.
    int availableInputCount() const { return availableInputCount_; }
    juce::String getInputError() const
    {
        if (inputError.isNotEmpty()) return inputError;
        if (inputUnavailable_.load(std::memory_order_relaxed))
            return "An input became unavailable.";
        return {};
    }

    int getLatencySamples() const { return latencySamples.load(std::memory_order_relaxed); }
    int getDeviceBufferSize() const { return deviceBufferSize; }
    double getOccupancy() const {
        if (capacity <= 0) return 0.0;
        return (double) readDelaySamples.load(std::memory_order_relaxed) / (double) capacity;
    }
    std::uint64_t getInputOverruns() const { return inputOverruns.load(std::memory_order_relaxed); }
    std::uint64_t getInputUnderruns() const { return inputUnderruns.load(std::memory_order_relaxed); }
    std::uint64_t getInputDroppedFrames() const { return inputDroppedFrames.load(std::memory_order_relaxed); }
    int getReadDelaySamples() const { return readDelaySamples.load(std::memory_order_relaxed); }
    // Round-trip measurement: the render thread starts the hunt on every
    // capture at the moment the impulse is placed on the clock output, so the
    // capture clock is snapshot before the impulse can come back. The result
    // is measured in captured samples, not in render blocks.
    void beginLatencyMeasurement() {
        latencyMeasureStartSample.store(totalWritten.load(std::memory_order_acquire),
                                        std::memory_order_relaxed);
        latencyImpulseDetected.store(false, std::memory_order_relaxed);
        latencyMeasureActive.store(true, std::memory_order_relaxed);
    }
    void endLatencyMeasurement() {
        latencyMeasureActive.store(false, std::memory_order_relaxed);
        latencyImpulseDetected.store(false, std::memory_order_relaxed);
    }
    bool isLatencyImpulseDetected() const { return latencyImpulseDetected.load(std::memory_order_relaxed); }
    std::uint64_t getLatencyImpulseInputSample() const { return latencyImpulseInputSample.load(std::memory_order_relaxed); }
    // Captured samples between the impulse leaving the render callback and
    // the captured copy of it; 0 until the impulse is seen.
    std::uint64_t getLatencyRoundTripSamples() const {
        const auto start = latencyMeasureStartSample.load(std::memory_order_relaxed);
        const auto hit = latencyImpulseInputSample.load(std::memory_order_relaxed);
        return hit > start ? hit - start : 0;
    }

    // Copies physicalChannels (1 entry = mono, 2 = stereo) into dest channels
    // 0/1 in order. dest must already be sized by the caller — no resizing
    // here (audio thread). A missing physical input leaves its dest channel
    // silent and returns false. `epoch` identifies one render pass: channels
    // sharing this capture get the same window instead of consuming it twice.
    bool pull(juce::AudioBuffer<float>& dest, int frames,
              const std::vector<int>& physicalChannels, std::uint64_t epoch);
    bool pull(juce::AudioBuffer<float>& dest, int frames, std::uint64_t epoch)
    {
        static const std::vector<int> stereoPassthrough{0, 1};
        return pull(dest, frames, stereoPassthrough, epoch);
    }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels, float* const* outputChannelData, int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    juce::AudioDeviceManager manager;
    juce::AudioBuffer<float> storage;
    int capacity = 0;
    // Producer state (input device callback only).
    int writePos = 0;
    // Published total; readers use it as the ring's high-water mark without
    // ever taking a lock, so a capture callback can never block or be dropped.
    std::atomic<std::uint64_t> totalWritten{0};
    // Reader state (render/audio thread only).
    pluginrack::CaptureReadCursor readCursor;
    float lastPlayed_[2] = {0.0f, 0.0f};
    // Declick: remember the epoch whose window was short and the next full
    // epoch that should ramp in. Epoch-scoped so every channel sharing this
    // capture receives the same fade.
    std::uint64_t lastUnderrunEpoch_ = 0;
    std::uint64_t fadeInEpoch_ = 0;
    std::atomic<int> readDelaySamples{0};
    double sampleRate = 0.0;
    juce::String error;
    juce::String inputError;
    juce::String activeDeviceType_;
    int inputChannelCount_ = 0;
    int availableInputCount_ = 0;
    std::atomic<bool> inputUnavailable_{false};
    int deviceInputLatency = 0;
    int deviceBufferSize = 0;
    std::atomic<int> latencySamples{0};
    std::atomic<std::uint64_t> inputOverruns{0};
    std::atomic<std::uint64_t> inputUnderruns{0};
    std::atomic<std::uint64_t> inputDroppedFrames{0};
    // Round-trip latency measurement support.
    std::atomic<bool> latencyMeasureActive{false};
    std::atomic<bool> latencyImpulseDetected{false};
    // Capture-clock position at the moment the impulse was emitted.
    std::atomic<std::uint64_t> latencyMeasureStartSample{0};
    std::atomic<std::uint64_t> latencyImpulseInputSample{0};
};

class OutputPlayback : public juce::AudioIODeviceCallback {
public:
    OutputPlayback();
    ~OutputPlayback() override;

    juce::String open(const juce::String& preferredDeviceName,
                      double engineSampleRate,
                      juce::String& errorOut,
                      int requestedBufferSize = 0,
                      // FIFO depth to keep filled, in device buffers. The
                      // clock output renders and drains in the same callback,
                      // so it needs far less headroom than a stream drained
                      // by an independent device clock.
                      int targetFillBlocks = 4,
                      // Blocks written before the device callback can read.
                      // -1 = same as targetFillBlocks.
                      int primeBlocks = -1,
                      // Try exclusive mode before the shared modes.
                      bool preferExclusive = false,
                      // Maximum render block that will be pushed into this
                      // FIFO. Independent output buffers can be smaller than
                      // the clock block, so the FIFO must still fit a whole
                      // render block.
                      int renderBlockFrames = 0);
    void close();

    double getSampleRate() const { return sampleRate; }
    int getBufferSize() const { return bufferSize; }
    double getOccupancy() const {
        if (capacity <= 0) return 0.0;
        return (double) fillFrames.load(std::memory_order_relaxed) /
               (double) juce::jmax(1, capacity);
    }
    int getFillFrames() const { return fillFrames.load(std::memory_order_relaxed); }
    int getTargetFillFrames() const { return targetFillFrames; }
    juce::String getError() const { return error; }
    // Device type the last successful open() selected (e.g. "Windows Audio
    // (Low Latency Mode)"). Diagnostics only; empty while closed.
    juce::String getActiveDeviceType() const { return activeDeviceType_; }

    int getLatencySamples() const { return latencySamples.load(std::memory_order_relaxed); }
    std::uint64_t getOutputUnderruns() const { return outputUnderruns.load(std::memory_order_relaxed); }
    std::uint64_t getOutputDroppedFrames() const { return outputDroppedFrames.load(std::memory_order_relaxed); }
    std::uint64_t getOutputLowWatermarks() const { return outputLowWatermarks.load(std::memory_order_relaxed); }

    void setRenderHook(std::function<void(int)> hook) { renderHook = std::move(hook); }
    void setRenderActive(bool active) { renderActive = active; }

    void push(const juce::AudioBuffer<float>& src, int frames);
    // Fills the FIFO with a fade-to-silence block. Used by the engine's
    // render-miss fallback: keeping the FIFO at its target fill means a UI
    // stall turns into one clean dropout instead of starving the device and
    // stuttering the last block. The next real push fades back in.
    void pushSilence(int frames);

    float* const* getDeviceOutputChannels() const { return currentOutputChannels; }
    int getNumDeviceOutputChannels() const { return currentNumOutputChannels; }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels, float* const* outputChannelData, int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    juce::AudioDeviceManager manager;
    // Guards fifo/storage against close() racing a push from the clock
    // device's render thread. push() only tries the lock and drops the block
    // when contended; the audio thread never blocks.
    juce::SpinLock fifoLock;
    std::unique_ptr<juce::AbstractFifo> fifo;
    juce::AudioBuffer<float> storage;
    int capacity = 0;
    int bufferSize = 256;
    int renderBlockFrames_ = 0;
    int targetFillFrames = 0;
    double sampleRate = 0.0;
    juce::String error;
    juce::String activeDeviceType_;
    int deviceOutputLatency = 0;
    std::atomic<int> latencySamples{0};
    std::atomic<int> fillFrames{0};
    // Set while close() is retiring this stream: its callback may still run
    // until the device thread stops, but it must not read or count a FIFO the
    // engine no longer feeds.
    std::atomic<bool> closing_{false};
    std::atomic<std::uint64_t> outputUnderruns{0};
    std::atomic<std::uint64_t> outputDroppedFrames{0};
    std::atomic<std::uint64_t> outputLowWatermarks{0};
    // Declick state. pushFadeInPending is only touched by push() /
    // pushSilence() (clock render thread); deviceFadeInRemaining only by the
    // device callback.
    bool pushFadeInPending = false;
    int deviceFadeInRemaining = 0;
    bool lowWatermarkActive = false;
    float lastPushedSample[2] = {0.0f, 0.0f};
    std::function<void(int)> renderHook;
    std::atomic<bool> renderActive{true};
    float* const* currentOutputChannels = nullptr;
    int currentNumOutputChannels = 0;
};
