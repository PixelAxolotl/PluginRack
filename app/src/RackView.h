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
    bool insertActivated(AudioEngine::InsertSlot* insert) override;

private:
    AudioEngine::Channel* currentChannel();
    AudioEngine::Output* currentOutput();
    const std::vector<std::unique_ptr<AudioEngine::InsertSlot>>* currentInserts() const;
    AudioEngine::InsertSlot* findInsert(std::uint32_t insertId);
    void ensureSelection();
    void refreshHeader();
    void refreshSlots();
    void refreshEditor();
    bool dockedHostAlive() const;
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
    juce::Label insertsHeader;
    EqButton eqButton{"EQ"};
    EqButton compressorButton{"COMP"};
    EqButton limiterButton{"LIMIT"};
    EqButton gateButton{"GATE"};
    EQWindow* eqWindow = nullptr;
    DynamicsWindow* dynamicsWindow = nullptr;
    bool eqFocused = false;
    bool dynamicsFocused = false;
    juce::Viewport slotViewport;
    juce::Component slotContent;
    juce::OwnedArray<InsertSlotComponent> slots;
    juce::Label emptySlotsLabel;
    juce::TextButton addVstButton{"+ Add VST3"};
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RackView)
};