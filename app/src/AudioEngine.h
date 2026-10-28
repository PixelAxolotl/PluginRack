#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <memory>
#include <vector>

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
        juce::String inputDeviceName;
        juce::String inputError;
        std::unique_ptr<InputCapture> input;
    };

    struct Output {
        std::uint32_t id = 0;
        juce::String name;
        juce::Atomic<float> peak{0.0f};
        juce::Atomic<float> gainDb{0.0f};
        juce::Atomic<bool> muted{false};
        juce::AudioBuffer<float> buffer;
        juce::String outputDeviceName;
        juce::String outputError;
        std::unique_ptr<OutputPlayback> output;
    };

    struct Route {
        std::uint32_t channelId = 0;
        std::uint32_t outputId = 0;
        juce::Atomic<float> sendGainDb{0.0f};
    };

    AudioEngine();

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate);
    void render(int frames);
    void releaseResources();

    void addChannel();
    void addOutput();
    juce::String setChannelInputDevice(std::uint32_t channelId, const juce::String& deviceName);
    juce::String setOutputDevice(std::uint32_t outputId, const juce::String& deviceName);
    juce::String getClockDescription() const;
    juce::StringArray getAvailableInputDevices() const;
    juce::StringArray getAvailableOutputDevices() const;
    juce::String addPlugin(std::uint32_t channelId, const juce::File& file);
    void removePlugin(std::uint32_t channelId, std::uint32_t insertId);
    void setPluginBypassed(std::uint32_t channelId, std::uint32_t insertId, bool bypassed);

    void setChannelGain(std::uint32_t channelId, float gainDb);
    void setChannelTrim(std::uint32_t channelId, float trimDb);
    void setChannelPan(std::uint32_t channelId, float pan);
    void setChannelMuted(std::uint32_t channelId, bool muted);
    void setChannelSoloed(std::uint32_t channelId, bool soloed);
    void setSendGain(std::uint32_t channelId, std::uint32_t outputId, float gainDb);
    float getSendGain(std::uint32_t channelId, std::uint32_t outputId) const;
    void setOutputGain(std::uint32_t outputId, float gainDb);
    void setOutputMuted(std::uint32_t outputId, bool muted);

    const std::vector<std::unique_ptr<Channel>>& getChannels() const;
    const std::vector<std::unique_ptr<Output>>& getOutputs() const;
    const std::vector<std::unique_ptr<Route>>& getRoutes() const;

    pluginrack::Scene makeScene() const;
    juce::String restoreScene(const pluginrack::Scene& scene);

private:
    Channel* findChannel(std::uint32_t channelId);
    InsertSlot* findInsert(std::uint32_t channelId, std::uint32_t insertId);
    static float decibelsToGain(float gainDb);

    std::vector<std::unique_ptr<Channel>> channels;
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<std::unique_ptr<Route>> routes;
    pluginrack::MeterBank meters;
    mutable juce::CriticalSection stateLock;
    std::uint32_t nextChannelId = 1;
    std::uint32_t nextOutputId = 1;
    std::uint32_t nextInsertId = 1;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 256;
    std::uint32_t clockOutputId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
