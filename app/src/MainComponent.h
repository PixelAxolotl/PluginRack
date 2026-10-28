#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "MixerComponent.h"
#include "RoutingMatrixComponent.h"

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
    void saveScene();
    void loadScene(bool showMissingMessage);
    void refreshViews();
    void showError(const juce::String& message);

    AudioEngine audioEngine;
    MixerComponent mixer;
    RoutingMatrixComponent routingMatrix;
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    juce::AudioDeviceSelectorComponent deviceSelector;
    juce::TextButton saveButton{"Save Scene"};
    juce::TextButton loadButton{"Load Scene"};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
