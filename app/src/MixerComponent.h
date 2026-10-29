#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "ChannelStrip.h"
#include "DeviceControl.h"
#include "DeviceMenu.h"
#include "LevelMeter.h"
#include "Theme.h"

class MixerComponent final : public juce::Component {
public:
    explicit MixerComponent(AudioEngine& engine);

    void resized() override;
    void rebuild();

    std::function<void()> onStructureChanged;

private:
    class OutputStrip final : public juce::Component {
    public:
        OutputStrip(AudioEngine::Output& output, AudioEngine& engine);
        void resized() override;
        void refresh();

    private:
        AudioEngine::Output& output;
        AudioEngine& audioEngine;
        DeviceControl deviceControl;
        juce::Slider gainSlider;
        LevelMeter meter;
        juce::TextButton muteButton{"Mute"};
    };

    AudioEngine& audioEngine;
    juce::TextButton addChannelButton{"+ Ch"};
    juce::TextButton addOutputButton{"+ Out"};
    juce::Viewport channelViewport;
    juce::Component stripContent;
    juce::OwnedArray<ChannelStrip> strips;

    juce::Component masterPanel;
    juce::Label masterHeader{"", "MASTER"};
    juce::Viewport masterViewport;
    juce::Component masterContent;
    juce::OwnedArray<OutputStrip> outputStrips;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerComponent)
};