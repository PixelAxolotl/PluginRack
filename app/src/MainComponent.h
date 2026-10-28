#pragma once

#include <JuceHeader.h>

#include "PluginHost.h"

class MainComponent final : public juce::AudioAppComponent {
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class PeakMeter final : public juce::Component,
                            private juce::Timer {
    public:
        explicit PeakMeter(juce::Atomic<float>& source);

        void paint(juce::Graphics& g) override;

    private:
        void timerCallback() override;

        juce::Atomic<float>& levelSource;
        float displayedLevel = 0.0f;
    };

    void choosePlugin();
    void loadPluginFile(const juce::File& file, bool showErrors);
    void saveScene();
    void restoreScene();
    void showError(const juce::String& message);

    PluginHost pluginHost;
    juce::Atomic<float> outputLevel{0.0f};
    PeakMeter levelMeter;
    juce::AudioDeviceSelectorComponent deviceSelector;
    juce::TextButton loadButton{"Load VST3"};
    juce::TextButton saveButton{"Save Scene"};
    juce::Label pluginLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
