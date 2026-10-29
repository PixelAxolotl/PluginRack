#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>

namespace DeviceEnumeration {
juce::StringArray inputs();
juce::StringArray outputs();
}

class InputCapture : public juce::AudioIODeviceCallback {
public:
    InputCapture();
    ~InputCapture() override;

    juce::String open(const juce::String& preferredDeviceName,
                      double engineSampleRate,
                      juce::String& errorOut);
    void close();

    double getSampleRate() const { return sampleRate; }
    juce::String getError() const { return error; }

    int getLatencySamples() const { return latencySamples.load(std::memory_order_relaxed); }

    void pull(juce::AudioBuffer<float>& dest, int frames);

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels, float* const* outputChannelData, int numOutputChannels, int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    juce::AudioDeviceManager manager;
    juce::SpinLock lock;
    std::unique_ptr<juce::AbstractFifo> fifo;
    juce::AudioBuffer<float> storage;
    int capacity = 0;
    double sampleRate = 0.0;
    juce::String error;
    int deviceInputLatency = 0;
    int deviceBufferSize = 0;
    std::atomic<int> latencySamples{0};
};

class OutputPlayback : public juce::AudioIODeviceCallback {
public:
    OutputPlayback();
    ~OutputPlayback() override;

    juce::String open(const juce::String& preferredDeviceName,
                      double engineSampleRate,
                      juce::String& errorOut);
    void close();

    double getSampleRate() const { return sampleRate; }
    int getBufferSize() const { return bufferSize; }
    juce::String getError() const { return error; }

    int getLatencySamples() const { return latencySamples.load(std::memory_order_relaxed); }

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
    std::function<void(int)> renderHook;
    std::atomic<bool> renderActive{true};
    float* const* currentOutputChannels = nullptr;
    int currentNumOutputChannels = 0;
};

