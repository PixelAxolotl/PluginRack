#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "Theme.h"

class SettingsView final : public juce::Component {
public:
    explicit SettingsView(AudioEngine& engine);

    void resized() override;
    void refresh();

private:
    AudioEngine& audioEngine;

    juce::Label titleLabel;
    juce::Label clockLabel;
    juce::Label latencyLabel;
    juce::Label bufferLabel;
    juce::Label errorLabel;

    juce::Label bufferSizeLabel;
    juce::ComboBox bufferSizeCombo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsView)
};