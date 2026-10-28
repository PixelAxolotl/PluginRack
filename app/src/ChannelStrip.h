#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "DeviceControl.h"
#include "DeviceMenu.h"
#include "LevelMeter.h"
#include "Theme.h"

class ChannelStrip final : public juce::Component {
public:
    ChannelStrip(AudioEngine::Channel& channel, AudioEngine& engine);

    void resized() override;
    void paint(juce::Graphics& g) override;
    void refreshInserts();
    void refreshRouting();

private:
    class InsertSlotComponent final : public juce::Component {
    public:
        InsertSlotComponent(ChannelStrip& owner, int slotIndex);

        void setInsert(const AudioEngine::InsertSlot* insert);
        void resized() override;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent&) override;

    private:
        ChannelStrip& owner;
        int slotIndex = 0;
        const AudioEngine::InsertSlot* insert = nullptr;
        juce::Label indexLabel;
        juce::Label nameLabel;
        juce::TextButton bypassButton{"B"};
        juce::TextButton removeButton{"x"};
    };

    void choosePlugin();
    void layoutSlots();

    AudioEngine::Channel& channel;
    AudioEngine& audioEngine;
    DeviceControl deviceControl;
    juce::Label vstHeader{"", "VST RACK"};

    juce::OwnedArray<InsertSlotComponent> slots;
    juce::TextButton addVstButton{"+ Add VST3"};

    juce::Label levelLabel{"", "Level"};
    juce::Slider levelKnob;
    juce::Label panLabel{"", "Pan"};
    juce::Slider panKnob;

    juce::OwnedArray<juce::TextButton> routingButtons;

    juce::Label faderLabel{"", "dB"};
    juce::Slider faderSlider;
    LevelMeter levelMeter;

    juce::TextButton muteButton{"Mute"};
    juce::TextButton soloButton{"Solo"};

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStrip)
};