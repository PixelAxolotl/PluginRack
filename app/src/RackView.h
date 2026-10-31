#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "AudioEngine.h"
#include "DynamicsWindow.h"
#include "EQWindow.h"
#include "InsertHost.h"
#include "LevelMeter.h"
#include "Theme.h"

// Channel-centric rack view: 4 columns - channel strip | insert chain (EQ + VSTs)
// | docked plugin editor | output routing. Reuses Mixer fader/meter/EqButton/slots.
class RackView final : public juce::Component, public IInsertHost {
public:
    explicit RackView(AudioEngine& engine);
    ~RackView() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void rebuild();
    void mouseDown(const juce::MouseEvent& e) override;

    void setSelectedChannel(int channelId, bool isOutput);

    // Releases the docked plugin editor before the engine destroys the
    // strip that owns its plugin instance (scene restore). The editor's
    // destructor calls back into its processor, so the processor must
    // still be alive when it is deleted.
    void closeDockedEditor();

    bool hasChainSelected() const { return selectedId >= 0; }
    std::uint32_t selectedChainId() const;
    bool selectedChainIsOutput() const { return selectedIsOutput; }

    std::function<void()> onJumpToMixer;
    std::function<void()> onStructureChanged;

    // IInsertHost
    juce::String insertPlugin(const juce::File& file) override;
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
    bool insertActivated(AudioEngine::InsertSlot* insert) override;

private:
    AudioEngine::Channel* currentChannel();
    AudioEngine::Output* currentOutput();
    const std::vector<std::unique_ptr<AudioEngine::InsertSlot>>* currentInserts() const;
    AudioEngine::InsertSlot* findInsert(std::uint32_t insertId);
    void ensureSelection();
    void refreshHeader();
    void refreshSlots();
    int chainBoundaryY(int targetIndex) const;
    void refreshEditor();
    bool dockedHostAlive() const;
    // Focus switches only hide the docked plugin editor: destroying and
    // recreating a VST UI on every EQ/dynamics switch stalls the message
    // thread and some plugins hang while being torn down.
    void hideDockedEditor();
    void showDockedEditor();
    void jumpToMixer();

    void rebuildChannelCombo();
    void focusEq();
    void closeEqWindow();
    void focusDynamics(pluginrack::DynamicsEffectType type);
    void closeDynamicsWindow();
    void toggleDynamicsBypass(pluginrack::DynamicsEffectType type);
    void refreshDynamicsButtons();
    void renameSelectedChain();
    void refreshEqButton();
    void refreshOutputButtons();

    AudioEngine& audioEngine;
    int selectedId = -1;
    bool selectedIsOutput = false;
    std::uint32_t focusedInsertId = 0;

    // Channel selector
    juce::ComboBox channelCombo;
    std::vector<int> comboIds;
    std::vector<char> comboIsOutput;

    // Left channel strip
    juce::Label nameLabel;
    juce::Label subLabel;
    juce::Label gainCaption;
    juce::Slider gainSlider;   // vertical
    juce::Label trimCaption;
    juce::Slider trimKnob;     // gain/trim rotary
    std::unique_ptr<LevelMeter> meter;
    juce::Label panCaption;
    juce::Slider panSlider;    // rotary
    juce::TextButton muteButton{"Mute"};
    juce::TextButton soloButton{"Solo"};
    juce::TextButton mixerButton{"Mixer"};

    // Plugin column
    juce::Label fxHeader;
    juce::Label insertsHeader;
    EqButton gateButton{"GATE"};
    EqButton eqButton{"EQ"};
    EqButton compressorButton{"COMP"};
    EQWindow* eqWindow = nullptr;
    DynamicsWindow* dynamicsWindow = nullptr;
    bool eqFocused = false;
    bool dynamicsFocused = false;
    juce::Viewport slotViewport;
    juce::Component slotContent;
    juce::OwnedArray<InsertSlotComponent> slots;
    int chainDropTarget = -1;
    int chainRowCount = 0;
    juce::Label chainMarker;
    std::unique_ptr<juce::FileChooser> fileChooser;

    // Center editor
    juce::Viewport editorViewport;
    juce::Component editorHolder;
    juce::Label noEditorLabel;
    juce::TextButton noEditorBypass{"Bypass"};
    juce::Label emptyLabel;

    // Right output routing
    juce::Label outputHeader;
    juce::OwnedArray<juce::TextButton> outputButtons;

    // Docked editor
    juce::AudioProcessorEditor* dockedEditor = nullptr;
    PluginHost* dockedHost = nullptr;
    // Size the plugin asked for when it was docked; layout reuses it so the
    // bay can never stretch the editor to fill.
    juce::Rectangle<int> dockedEditorSize;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RackView)
};