#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "DeviceControl.h"
#include "DeviceMenu.h"
#include "InsertHost.h"
#include "LevelMeter.h"
#include "MiniButton.h"
#include "Theme.h"

class ChannelStrip final : public juce::Component, public IInsertHost {
public:
    ChannelStrip(AudioEngine::Channel& channel, AudioEngine& engine);

    void resized() override;
    void paint(juce::Graphics& g) override;
    void refreshInserts();
    void refreshRouting();
    void rebuildRoutingButtons();

    // IInsertHost
    juce::String insertPlugin(const juce::File&) override;
    void erasePlugin(std::uint32_t insertId) override;
    void setPluginBypass(std::uint32_t insertId, bool bypassed) override;
    void refreshAfterInsertChange() override;
    void openPicker() override;

public:
    std::function<void()> onRemoveRequested;

private:
    void layoutSlots();

    AudioEngine::Channel& channel;
    AudioEngine& audioEngine;
    DeviceControl deviceControl;
    juce::Label channelLabel{"", ""};
    MiniButton removeChannelButton{juce::String::fromUTF8("\xc3\x97")};
    juce::Label vstHeader{"", "VST RACK"};

    juce::OwnedArray<InsertSlotComponent> slots;
    juce::TextButton addVstButton{"+ Add VST3"};

    juce::Label levelLabel{"", "Level"};
    juce::Slider levelKnob;
    juce::Label panLabel{"", "Pan"};
    juce::Slider panKnob;

    juce::OwnedArray<juce::TextButton> routingButtons;
    std::vector<std::uint32_t> routingOutputIds;

    juce::Label faderLabel{"", "dB"};
    juce::Slider faderSlider;
    LevelMeter levelMeter;

    juce::TextButton muteButton{"Mute"};
    juce::TextButton soloButton{"Solo"};

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStrip)
};