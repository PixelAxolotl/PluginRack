#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
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
        std::vector<std::unique_ptr<InsertSlot>> inserts;
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
        bool valid = false;
        bool approximate = true;
    };

    struct FifoDiagnostics {
        std::uint64_t inputOverruns = 0;
        std::uint64_t inputDroppedFrames = 0;
        std::uint64_t outputUnderruns = 0;
        std::uint64_t outputDroppedFrames = 0;
        double inputOccupancy = 0.0;
        double outputOccupancy = 0.0;
    };

    AudioEngine();

    void prepareToPlay(int maxBlockFrames);
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
    juce::String getClockDescription() const;
    juce::StringArray getAvailableInputDevices() const;
    juce::StringArray getAvailableOutputDevices() const;
    juce::String addPlugin(std::uint32_t channelId, const juce::File& file);
    void removePlugin(std::uint32_t channelId, std::uint32_t insertId);
    void setPluginBypassed(std::uint32_t channelId, std::uint32_t insertId, bool bypassed);
    juce::String addOutputPlugin(std::uint32_t outputId, const juce::File& file);
    void removeOutputPlugin(std::uint32_t outputId, std::uint32_t insertId);
    void setOutputPluginBypassed(std::uint32_t outputId, std::uint32_t insertId, bool bypassed);

    void setChannelGain(std::uint32_t channelId, float gainDb);
    void setChannelTrim(std::uint32_t channelId, float trimDb);
    void setChannelPan(std::uint32_t channelId, float pan);
    void setChannelMuted(std::uint32_t channelId, bool muted);
    void setChannelSoloed(std::uint32_t channelId, bool soloed);
    void setChannelName(std::uint32_t channelId, const juce::String& name);
    void setChannelLayout(std::uint32_t channelId, pluginrack::ChannelLayout layout);
    void setChannelInputChannels(std::uint32_t channelId, const std::vector<int>& inputChannels);
    void setSendGain(std::uint32_t channelId, std::uint32_t outputId, float gainDb);
    float getSendGain(std::uint32_t channelId, std::uint32_t outputId) const;
    void setOutputGain(std::uint32_t outputId, float gainDb);
    void setOutputMuted(std::uint32_t outputId, bool muted);

    const std::vector<std::unique_ptr<Channel>>& getChannels() const;
    const std::vector<std::unique_ptr<Output>>& getOutputs() const;
    const std::vector<std::unique_ptr<Route>>& getRoutes() const;

    LatencyReport getLatencyReport() const;

    FifoDiagnostics getFifoDiagnostics() const;

    pluginrack::Scene makeScene() const;
    juce::String restoreScene(const pluginrack::Scene& scene);

private:
    Channel* findChannel(std::uint32_t channelId);
    InsertSlot* findInsert(std::uint32_t channelId, std::uint32_t insertId);
    Output* findOutput(std::uint32_t outputId);
    InsertSlot* findOutputInsert(std::uint32_t outputId, std::uint32_t insertId);
    static float decibelsToGain(float gainDb);
    // Drops captures no channel references anymore (closes their device).
    // Requires stateLock.
    void pruneUnusedCaptures();

    struct SharedInputCapture {
        std::shared_ptr<InputCapture> capture;
        // Physical input count the device was opened with; >= a later request
        // means the capture can be reused even when the device itself offers
        // fewer inputs (missing ones stay silent and are reported).
        int requestedChannels = 0;
    };

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
    int maxBlockFrames_ = 256;
    std::atomic<std::uint64_t> oversizedBlockCount_{0};
    juce::AudioBuffer<float> silenceBuffer_;
    std::uint32_t clockOutputId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
