#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "Theme.h"

class SettingsView final : public juce::Component, private juce::Timer {
public:
    explicit SettingsView(AudioEngine& engine);

    void resized() override;
    void refresh();

private:
    void timerCallback() override;
    AudioEngine& audioEngine;

    juce::Label titleLabel;
    juce::Label clockLabel;
    juce::Label latencyLabel;
    juce::Label bufferLabel;
    juce::Label errorLabel;

    juce::Label inputBufferSizeLabel;
    juce::ComboBox inputBufferSizeCombo;
    juce::Label outputBufferSizeLabel;
    juce::ComboBox outputBufferSizeCombo;

    juce::TextButton measureButton { "Measure Round-Trip Latency" };
    juce::Label measuredLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsView)
};