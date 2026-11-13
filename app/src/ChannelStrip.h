#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "DeviceControl.h"
#include "DeviceMenu.h"
#include "EQWindow.h"
#include "DynamicsWindow.h"
#include "MultibandWindow.h"
#include "DeEsserWindow.h"
#include "LimiterWindow.h"
#include "InsertHost.h"
#include "LevelMeter.h"
#include "MiniButton.h"
#include "StripDialog.h"
#include "Theme.h"
#include "ValueEditSlider.h"

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
    void closeAllEffectWindows();
    void refreshEqButton();
    void openDynamicsWindow(pluginrack::DynamicsEffectType type);
    void toggleDynamicsBypass(pluginrack::DynamicsEffectType type);
    void openMultibandWindow();
    void openDeEsserWindow();
    void openLimiterWindow();
    void refreshDynamicsButtons();

    // IInsertHost
    juce::String insertPlugin(const juce::File&, int pluginUniqueId = 0) override;
    void erasePlugin(std::uint32_t insertId) override;
    void setPluginBypass(std::uint32_t insertId, bool bypassed) override;
    void reloadPlugin(std::uint32_t insertId) override;
    void refreshAfterInsertChange() override;
    void reorderInsert(std::uint32_t insertId, int targetIndex) override;
    void reorderChainItem(pluginrack::ChainItem item, int targetIndex) override;
    void previewChainItem(int targetIndex) override;
    void clearChainPreview() override;
    int chainDropIndexAt(juce::Point<int> parentPoint) override;
    void setBuiltInBypass(pluginrack::ChainItem item, bool bypassed) override;
    void activateBuiltIn(pluginrack::ChainItem item) override;
    void insertBuiltIn(pluginrack::ChainItem item, int chainIndex) override;
    void eraseBuiltIn(pluginrack::ChainItem item) override;

public:
    std::function<void()> onRemoveRequested;
    std::function<void()> rebuildCallback;
    std::function<void()> onSelected;
    std::function<void()> onDoubleClicked;

    // Channel reordering (mixer only): the grip at the strip's top-left reports
    // the drag in screen space; the owner maps it to a target index and owns the
    // resulting rebuild. Hidden unless the hosting board asks for it.
    std::function<void()> onDragHandleStarted;
    std::function<void(juce::Point<int> screenPosition)> onDragHandleMoved;
    std::function<void()> onDragHandleFinished;
    void setDragHandleVisible(bool visible);

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

private:
    void layoutSlots();
    int chainBoundaryY(int targetIndex) const;
    void applyInputMapping();
    void splitIntoMonoChannels();
    void rebuildInputCombos(int count);
    void paintDragHandle(juce::Graphics& g) const;

    AudioEngine::Channel& channel;
    AudioEngine& audioEngine;
    DeviceControl deviceControl;
    MiniButton removeChannelButton{juce::String::fromUTF8("\xc3\x97")};
    MiniButton splitButton{"Split"};
    juce::Label vstHeader{"", "VST RACK"};

    juce::TextButton inputChip;
    bool inputEditing = false;
    juce::Rectangle<int> dragHandleBounds;
    bool dragHandleVisible = false;
    bool draggingHandle = false;
    juce::ComboBox modeCombo;
    juce::ComboBox inputComboA;
    juce::ComboBox inputComboB;
    // How many entries the input combos currently hold, so they are rebuilt
    // only when the device's input count or the saved mapping changes.
    int inputComboCount = 0;

    juce::OwnedArray<InsertSlotComponent> slots;
    int chainDropTarget = -1;
    int chainRowCount = 0;

    juce::Label levelLabel{"", "Level"};
    ValueEditSlider levelKnob;
    juce::Label panLabel{"", "Pan"};
    ValueEditSlider panKnob;

    juce::OwnedArray<juce::TextButton> routingButtons;
    std::vector<std::uint32_t> routingOutputIds;

    juce::Label faderLabel{"", "dB"};
    ValueEditSlider faderSlider;
    LevelMeter levelMeter;

    juce::TextButton muteButton{"Mute"};
    juce::TextButton soloButton{"Solo"};
    EQWindow* eqWindow = nullptr;
    DynamicsWindow* dynamicsWindow = nullptr;
    MultibandWindow* multibandWindow = nullptr;
    DeEsserWindow* deEsserWindow = nullptr;
    LimiterWindow* limiterWindow = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStrip)
};