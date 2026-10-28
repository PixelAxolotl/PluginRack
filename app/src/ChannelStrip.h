#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "LevelMeter.h"

class ChannelStrip final : public juce::Component {
public:
    using AddPluginCallback = std::function<juce::String(std::uint32_t, const juce::File&)>;
    using RemovePluginCallback = std::function<void(std::uint32_t, std::uint32_t)>;
    using BypassPluginCallback = std::function<void(std::uint32_t, std::uint32_t, bool)>;

    ChannelStrip(AudioEngine::Channel& channel, AudioEngine& engine);

    void resized() override;
    void refreshInserts();

private:
    class InsertRow final : public juce::Component {
    public:
        InsertRow(const AudioEngine::InsertSlot& insert, ChannelStrip& owner);

        void resized() override;

    private:
        juce::Label nameLabel;
        juce::TextButton bypassButton{"Bypass"};
        juce::TextButton removeButton{"X"};
    };

    void choosePlugin();
    void layoutInsertRows();

    AudioEngine::Channel& channel;
    AudioEngine& audioEngine;
    juce::Label nameLabel;
    juce::Label gainLabel{"", "Gain"};
    juce::Slider gainSlider;
    juce::Label panLabel{"", "Pan"};
    juce::Slider panSlider;
    juce::TextButton muteButton{"Mute"};
    juce::TextButton soloButton{"Solo"};
    LevelMeter levelMeter;
    juce::TextButton addPluginButton{"Add VST3"};
    juce::Viewport insertViewport;
    juce::Component insertContent;
    juce::OwnedArray<InsertRow> insertRows;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStrip)
};
