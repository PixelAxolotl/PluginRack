#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "MixerComponent.h"
#include "RoutingMatrixComponent.h"
#include "Theme.h"

class MainComponent final : public juce::Component, private juce::Timer {
public:
    MainComponent();
    ~MainComponent() override = default;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void saveScene();
    void loadScene(bool showMissingMessage);
    void refreshViews();
    void showError(const juce::String& message);
    void timerCallback() override;

    AudioEngine audioEngine;
    MixerComponent mixer;
    juce::TextButton saveButton{"Save"};
    juce::TextButton loadButton{"Load"};
    juce::TextButton menuButton{"Menu"};
    juce::Label latencyLabel;
    double smoothedLatencyMs = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
