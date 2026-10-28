#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "ChannelStrip.h"

class MixerComponent final : public juce::Component {
public:
    explicit MixerComponent(AudioEngine& engine);

    void resized() override;
    void rebuild();

    std::function<void()> onStructureChanged;

private:
    AudioEngine& audioEngine;
    juce::TextButton addChannelButton{"Add channel"};
    juce::TextButton addOutputButton{"Add output"};
    juce::Viewport channelViewport;
    juce::Component stripContent;
    juce::OwnedArray<ChannelStrip> strips;
    juce::Component outputsPanel;
    juce::Label outputsHeader{"", "Outputs"};
    juce::OwnedArray<juce::Label> outputLabels;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerComponent)
};
