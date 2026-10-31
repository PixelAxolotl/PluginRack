#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "DeviceControl.h"
#include "DeviceMenu.h"
#include "EQWindow.h"
#include "DynamicsWindow.h"
#include "InsertHost.h"
#include "LevelMeter.h"
#include "MiniButton.h"
#include "Theme.h"

class ChannelStrip final : public juce::Component, public IInsertHost {
public:
    ChannelStrip(AudioEngine::Channel& channel, AudioEngine& engine);
    ~ChannelStrip() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void refreshInserts();
    void refreshRouting();
    void rebuildRoutingButtons();
    void refreshInput();
    void openEqWindow();
    void closeEqWindow();
    void refreshEqButton();
    void openDynamicsWindow(pluginrack::DynamicsEffectType type);
    void toggleDynamicsBypass(pluginrack::DynamicsEffectType type);
    void refreshDynamicsButtons();

    // IInsertHost
    juce::String insertPlugin(const juce::File&) override;
    void erasePlugin(std::uint32_t insertId) override;
    void setPluginBypass(std::uint32_t insertId, bool bypassed) override;
    void refreshAfterInsertChange() override;
    void openPicker() override;
    void reorderInsert(std::uint32_t insertId, int targetIndex) override;
    void reorderChainItem(pluginrack::ChainItem item, int targetIndex) override;
    void previewChainItem(int targetIndex) override;
    void clearChainPreview() override;
    int chainDropIndexAt(juce::Point<int> parentPoint) override;
    void setBuiltInBypass(pluginrack::ChainItem item, bool bypassed) override;
    void activateBuiltIn(pluginrack::ChainItem item) override;

public:
    std::function<void()> onRemoveRequested;
    std::function<void()> rebuildCallback;
    std::function<void()> onSelected;
    std::function<void()> onDoubleClicked;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

private:
    void layoutSlots();
    int chainBoundaryY(int targetIndex) const;
    void applyInputMapping();
    void splitIntoMonoChannels();

    AudioEngine::Channel& channel;
    AudioEngine& audioEngine;
    DeviceControl deviceControl;
    MiniButton removeChannelButton{juce::String::fromUTF8("\xc3\x97")};
    MiniButton splitButton{"Split"};
    juce::Label vstHeader{"", "VST RACK"};

    juce::TextButton inputChip;
    bool inputEditing = false;
    juce::ComboBox modeCombo;
    juce::ComboBox inputComboA;
    juce::ComboBox inputComboB;
    juce::Label inputErrorLabel{"", ""};

    juce::OwnedArray<InsertSlotComponent> slots;
    int chainDropTarget = -1;
    int chainRowCount = 0;

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
    EqButton gateButton{"GATE"};
    EqButton eqButton{"EQ"};
    EqButton compressorButton{"COMP"};
    EQWindow* eqWindow = nullptr;
    DynamicsWindow* dynamicsWindow = nullptr;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStrip)
};