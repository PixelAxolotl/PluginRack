#pragma once

#include <JuceHeader.h>

#include <atomic>
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
                      int requestedBufferSize = 0);
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
    std::uint64_t getInputOverruns() const { return inputOverruns.load(std::memory_order_relaxed); }
    std::uint64_t getInputDroppedFrames() const { return inputDroppedFrames.load(std::memory_order_relaxed); }

    // Copies physicalChannels (1 entry = mono, 2 = stereo) into dest channels
    // 0/1 in order. dest must already be sized by the caller — no resizing
    // here (audio thread). A missing physical input leaves its dest channel
    // silent and returns false.
    bool pull(juce::AudioBuffer<float>& dest, int frames,
              const std::vector<int>& physicalChannels);
    bool pull(juce::AudioBuffer<float>& dest, int frames)
    {
        static const std::vector<int> stereoPassthrough{0, 1};
        return pull(dest, frames, stereoPassthrough);
    }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels, float* const* outputChannelData, int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    juce::AudioDeviceManager manager;
    juce::SpinLock lock;
    juce::AudioBuffer<float> storage;
    int capacity = 0;
    int writePos = 0;
    int totalWritten = 0;
    double sampleRate = 0.0;
    juce::String error;
    juce::String inputError;
    int inputChannelCount_ = 0;
    int availableInputCount_ = 0;
    std::atomic<bool> inputUnavailable_{false};
    int deviceInputLatency = 0;
    int deviceBufferSize = 0;
    std::atomic<int> latencySamples{0};
    std::atomic<std::uint64_t> inputOverruns{0};
    std::atomic<std::uint64_t> inputDroppedFrames{0};
};

class OutputPlayback : public juce::AudioIODeviceCallback {
public:
    OutputPlayback();
    ~OutputPlayback() override;

    juce::String open(const juce::String& preferredDeviceName,
                      double engineSampleRate,
                      juce::String& errorOut,
                      int requestedBufferSize = 0);
    void close();

    double getSampleRate() const { return sampleRate; }
    int getBufferSize() const { return bufferSize; }
    juce::String getError() const { return error; }

    int getLatencySamples() const { return latencySamples.load(std::memory_order_relaxed); }
    std::uint64_t getOutputUnderruns() const { return outputUnderruns.load(std::memory_order_relaxed); }
    std::uint64_t getOutputDroppedFrames() const { return outputDroppedFrames.load(std::memory_order_relaxed); }

    void setRenderHook(std::function<void(int)> hook) { renderHook = std::move(hook); }
    void setRenderActive(bool active) { renderActive = active; }

    void push(const juce::AudioBuffer<float>& src, int frames);

    float* const* getDeviceOutputChannels() const { return currentOutputChannels; }
    int getNumDeviceOutputChannels() const { return currentNumOutputChannels; }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels, float* const* outputChannelData, int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    juce::AudioDeviceManager manager;
    std::unique_ptr<juce::AbstractFifo> fifo;
    juce::AudioBuffer<float> storage;
    int capacity = 0;
    int bufferSize = 256;
    double sampleRate = 0.0;
    juce::String error;
    int deviceOutputLatency = 0;
    std::atomic<int> latencySamples{0};
    std::atomic<std::uint64_t> outputUnderruns{0};
    std::atomic<std::uint64_t> outputDroppedFrames{0};
    std::function<void(int)> renderHook;
    std::atomic<bool> renderActive{true};
    float* const* currentOutputChannels = nullptr;
    int currentNumOutputChannels = 0;
};

