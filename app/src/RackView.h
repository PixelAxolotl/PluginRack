#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "AudioEngine.h"
#include "DynamicsWindow.h"
#include "EQWindow.h"
#include "InsertHost.h"
#include "InsertMiniView.h"
#include "LevelMeter.h"
#include "Theme.h"

// Channel-centric rack view: 4 columns - channel strip | insert chain (EQ + VSTs)
// | docked plugin editor | output routing. Reuses Mixer fader/meter/EqButton/slots.
class RackView final : public juce::Component, public IInsertHost, private juce::Timer {
public:
    explicit RackView(AudioEngine& engine);
    ~RackView() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void visibilityChanged() override;
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
    // Maps a row index in the visible list to the chain index a drop lands on.
    // The dynamics pair is one row but two chain entries, so a drop after the
    // card has to skip past the second one.
    int chainIndexForRow(int row);
    void moveChainItemRaw(pluginrack::ChainItem item, int chainIndex);
    // Moves both rows of the pair so the merged card keeps them adjacent.
    void moveDynamicsPair(pluginrack::ChainItem item, int targetRowIndex);
    // Resolves a dynamics row to its effect. Scenes store the effect ids they
    // were saved with (the gate's has moved), so a mismatched id falls back to
    // the effect type the row names.
    const pluginrack::DynamicsEffect* dynamicsEffectFor(std::uint32_t itemId);
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
    // Card thumbnails: the timer refreshes the focused VST's snapshot and the
    // built-in graphs; the tap is re-armed every tick because another view can
    // switch it off.
    void timerCallback() override;
    void startMiniTimer();
    AudioEngine::AnalyzerTap* currentTap();
    void releaseMiniTap();

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
    // Cards that carry a mini view, so the timer can feed them without walking
    // the list. Cleared at the top of refreshSlots(), which rebuilds every row.
    InsertSlotComponent* eqCard = nullptr;
    // Compressor and gate share one card, so one pointer covers both.
    InsertSlotComponent* dynamicsCard = nullptr;
    std::map<std::uint32_t, InsertSlotComponent*> vstCards;
    // Last snapshot per insert. Kept after its card is rebuilt so a thumbnail
    // does not blink away every time the layout changes.
    std::map<std::uint32_t, juce::Image> vstThumbs;
    MiniSpectrum miniSpectrum;
    DynamicsMeterSim compressorMeter;
    DynamicsMeterSim gateMeter;
    std::vector<float> tapLeft;
    std::vector<float> tapRight;
    int miniTimerHz = 0;
    // Armed strip, held by id: a scene restore destroys the strip a raw pointer
    // would point at.
    std::uint32_t tapArmedId = 0;
    bool tapArmedIsOutput = false;
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