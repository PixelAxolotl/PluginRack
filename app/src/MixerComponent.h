#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "ChannelStrip.h"
#include "DeviceControl.h"
#include "EQWindow.h"
#include "InsertHost.h"
#include "DeviceMenu.h"
#include "LevelMeter.h"
#include "MiniButton.h"
#include "Theme.h"

class MixerComponent final : public juce::Component {
public:
    explicit MixerComponent(AudioEngine& engine);

    void resized() override;
    void rebuild();

    std::function<void()> onStructureChanged;

private:
    class OutputStrip final : public juce::Component, public IInsertHost {
    public:
        // Output EQ is keyed by scene-order position (Task 3 asymmetry), so the
        // strip captures its index at construction; rebuild() recreates strips
        // whenever the structure changes, keeping it fresh.
        OutputStrip(AudioEngine::Output& output, int outputIndex, AudioEngine& engine);
        ~OutputStrip() override;
        void resized() override;
        void refresh();
        void openEqWindow();
        void closeEqWindow();
        void refreshEqButton();
        int getOutputIndex() const { return outputIndex; }

        // IInsertHost
        juce::String insertPlugin(const juce::File&) override;
        void erasePlugin(std::uint32_t insertId) override;
        void setPluginBypass(std::uint32_t insertId, bool bypassed) override;
        void refreshAfterInsertChange() override;
        void openPicker() override;

        std::function<void()> onRemoveRequested;
        std::function<void()> onRenamed;

    private:
        void refreshInserts();

        AudioEngine::Output& output;
        int outputIndex = 0;
        AudioEngine& audioEngine;
        DeviceControl deviceControl;
        MiniButton removeOutputButton{juce::String::fromUTF8("\xc3\x97")};
        juce::ComboBox modeCombo;
        juce::TextButton flipButton{"Flip"};
        juce::OwnedArray<InsertSlotComponent> slots;
        juce::Label vstHeader{"", "VST RACK"};
        juce::TextButton addVstButton{"+ Add VST3"};
        std::unique_ptr<juce::FileChooser> fileChooser;
        juce::Slider gainSlider;
        LevelMeter meter;
        juce::TextButton muteButton{"Mute"};
        EqButton eqButton{"EQ"};
        EQWindow* eqWindow = nullptr;
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