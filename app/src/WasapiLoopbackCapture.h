#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <memory>

// Capture-only loopback device. Windows builds get the real WASAPI reader
// (see the .cpp); other platforms get a stub that reports "not supported".
class WasapiLoopbackDevice final : public juce::AudioIODevice
{
public:
    static constexpr const char* kDeviceName = "System loopback (WASAPI)";

    WasapiLoopbackDevice();
    ~WasapiLoopbackDevice() override;

    juce::StringArray getOutputChannelNames() override;    juce::StringArray getInputChannelNames() override;
    juce::Array<double> getAvailableSampleRates() override;
    juce::Array<int> getAvailableBufferSizes() override;
    int getDefaultBufferSize() override;
    juce::String open (const juce::BigInteger& inputChannels,
                       const juce::BigInteger& outputChannels,
                       double sampleRate, int bufferSizeSamples) override;
    void close() override;
    bool isOpen() override;
    void start (juce::AudioIODeviceCallback* callback) override;
    void stop() override;
    bool isPlaying() override;
    juce::String getLastError() override;
    int getCurrentBufferSizeSamples() override;
    double getCurrentSampleRate() override;
    int getCurrentBitDepth() override;
    juce::BigInteger getActiveOutputChannels() const override;
    juce::BigInteger getActiveInputChannels() const override;
    int getOutputLatencyInSamples() override;
    int getInputLatencyInSamples() override;

private:
    class Reader; // std::thread + COM pump, defined in the .cpp
    void fireCallback (const float* const* inputs, int numInputs) noexcept;
    std::unique_ptr<Reader> reader_;

    juce::String name_ = kDeviceName;
    juce::String lastError_;    juce::CriticalSection stateLock_;
    juce::AudioIODeviceCallback* callback_ = nullptr;
    std::atomic<bool> openFlag_ { false };
    std::atomic<bool> playingFlag_ { false };
    int numCaptureChannels_ = 2;
    int bufferSizeSamples_ = 0;
    double sampleRate_ = 0.0;
    int inputLatencySamples_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WasapiLoopbackDevice)
};

class WasapiLoopbackDeviceType final : public juce::AudioIODeviceType
{
public:
    static constexpr const char* kTypeName = "WASAPI Loopback";

    WasapiLoopbackDeviceType();
    void scanForDevices() override;
    juce::StringArray getDeviceNames (bool wantInputNames) const override;
    int getDefaultDeviceIndex (bool forInput) const override;
    int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override;
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice (const juce::String& outputDeviceName,
                                       const juce::String& inputDeviceName) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WasapiLoopbackDeviceType)
};

namespace WasapiLoopback {
// Registers the type once on this manager; returns the type name the
// device manager will report ("WASAPI Loopback"), or an empty string when the
// platform has no support.
juce::String registerWith (juce::AudioDeviceManager& manager);
} // namespace WasapiLoopback
