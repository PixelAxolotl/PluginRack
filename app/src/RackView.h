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
#include "NativeThumbnail.h"
#include "PluginKnobWatcher.h"
#include "Theme.h"
#include "TouchSlideBar.h"
#include "ValueEditSlider.h"

// Channel-centric rack view: 4 columns - channel strip | insert chain (EQ + VSTs)
// | docked plugin editor | output routing. Reuses Mixer fader/meter/EqButton/slots.
class RackView final : public juce::Component,
                       public IInsertHost,
                       private juce::Timer,
                       private juce::ComponentListener {
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
    // Releases the docked editor only when its insert is gone from the engine.
    // A structure change (adding an insert) must not tear a live editor down and
    // rebuild it: that costs a full UI re-create and some plugins hang on the
    // teardown of an editor that was just created.
    void releaseOrphanedDockedEditor();

    bool hasChainSelected() const { return selectedId >= 0; }
    std::uint32_t selectedChainId() const;
    bool selectedChainIsOutput() const { return selectedIsOutput; }

    std::function<void()> onJumpToMixer;
    std::function<void()> onStructureChanged;

    // IInsertHost
    juce::String insertPlugin(const juce::File& file, int pluginUniqueId = 0) override;
    void erasePlugin(std::uint32_t insertId) override;
    void setPluginBypass(std::uint32_t insertId, bool bypassed) override;
    void refreshAfterInsertChange() override;
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
    const std::vector<pluginrack::ChainItem>* currentChain() const;
    const std::vector<pluginrack::DynamicsEffect>* currentEffects() const;
    const pluginrack::DynamicsEffect* effectOfType(pluginrack::DynamicsEffectType type) const;
    AudioEngine::InsertSlot* findInsert(std::uint32_t insertId);
    // Maps a row index in the visible list to the chain index a drop lands on.
    // A card can cover two chain entries (the Dynamics card holds the EQ and
    // the compressor), so rows do not map one-to-one.
    int chainIndexForRow(int row);
    void moveChainItemRaw(pluginrack::ChainItem item, int chainIndex);
    // Moves every chain entry a card covers so the card keeps them adjacent.
    void moveChainCard(const std::vector<pluginrack::ChainItem>& items, int targetRowIndex);
    // Resolves a dynamics row to its effect. Scenes store the effect ids they
    // were saved with (the gate's has moved), so a mismatched id falls back to
    // the effect type the row names.
    const pluginrack::DynamicsEffect* dynamicsEffectFor(std::uint32_t itemId);
    void ensureSelection();
    void refreshHeader();
    void refreshSlots();
    int chainBoundaryY(int targetIndex) const;
    void refreshEditor();
    // Scales the docked plugin editor to fit the bay and centres it in it.
    void layoutDockedEditor();
    // Scales the docked editor so one axis fills the bay and the other fits,
    // keeping the plugin's own aspect ratio. The tighter of width/height wins,
    // so a wide UI (Ozone's EQ) is shrunk instead of clipped.
    void applyDockedEditorFit(const juce::Rectangle<int>& bay);
    // The editor's painted rectangle inside the bay: its bounds, multiplied by
    // the scale when a software-drawn editor was scaled through a transform.
    juce::Rectangle<int> dockedEditorVisualBounds() const;
    void componentMovedOrResized(juce::Component& component, bool wasMoved,
                                 bool wasResized) override;
    bool dockedHostAlive() const;
    // Focus switches only hide the docked plugin editor: destroying and
    // recreating a VST UI on every EQ/dynamics switch stalls the message
    // thread and some plugins hang while being torn down.
    void hideDockedEditor();
    void showDockedEditor();
    void jumpToMixer();

    void rebuildChannelCombo();
    void rebuildInputCombos(int count);
    void refreshInputControls();
    void applyInputMapping();
    void focusEq();
    void closeEqWindow();
    void focusDynamics(pluginrack::DynamicsEffectType type);
    void closeDynamicsWindow();
    void toggleDynamicsBypass(pluginrack::DynamicsEffectType type);
    void refreshDynamicsButtons();
    void renameSelectedChain();
    void refreshEqButton();
    void refreshOutputButtons();
    void refreshSourceList();
    // Card thumbnails: the timer refreshes the focused VST's snapshot and the
    // built-in graphs; the tap is re-armed every tick because another view can
    // switch it off.
    void timerCallback() override;
    void startMiniTimer();
    AudioEngine::AnalyzerTap* currentTap();
    void releaseMiniTap();
    void applyVstThumbnail(std::uint32_t insertId, const juce::Image& thumbnail);

    AudioEngine& audioEngine;
    int selectedId = -1;
    bool selectedIsOutput = false;
    std::uint32_t focusedInsertId = 0;

    // Channel selector
    // A ComboBox takes its text font from the LookAndFeel, so scaling just this
    // one control needs its own small override. Declared before the combo so it
    // outlives it.
    class SelectorLookAndFeel final : public juce::LookAndFeel_V4 {
    public:
        juce::Font getComboBoxFont(juce::ComboBox&) override {
            return juce::Font(juce::FontOptions(15.0f, juce::Font::bold));
        }
        juce::Font getPopupMenuFont() override {
            return juce::Font(juce::FontOptions(14.0f));
        }
    };
    SelectorLookAndFeel selectorLookAndFeel;
    juce::ComboBox channelCombo;
    juce::TextButton inputChip;
    bool inputEditing = false;
    juce::ComboBox modeCombo;
    juce::ComboBox inputComboA;
    juce::ComboBox inputComboB;
    // How many entries the input combos currently hold, so they are rebuilt
    // only when the device's input count or the saved mapping changes.
    int inputComboCount = 0;
    std::vector<int> comboIds;
    std::vector<char> comboIsOutput;

    // Left channel strip
    juce::Label nameLabel;
    juce::Label subLabel;
    juce::Label gainCaption;
    ValueEditSlider gainSlider;   // vertical
    juce::Label trimCaption;
    ValueEditSlider trimKnob;     // gain/trim rotary
    std::unique_ptr<LevelMeter> meter;
    juce::Label panCaption;
    ValueEditSlider panSlider;    // rotary
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
    // Compressor card: the merged Dynamics card when the gate is there too.
    InsertSlotComponent* dynamicsCard = nullptr;
    bool dynamicsCardPaired = false;
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

    // Center editor
    juce::Viewport editorViewport;
    juce::Component editorHolder;
    juce::Label noEditorLabel;
    juce::TextButton noEditorBypass{"Bypass"};
    juce::Label emptyLabel;

    // Right output routing
    juce::Label outputHeader;
    juce::OwnedArray<juce::TextButton> outputButtons;
    // Shown instead of the sends when an output is selected: the input channels
    // that feed it, each with the device it is receiving from. Painted as rows
    // because a row carries two strings.
    class SourceRowList final : public juce::Component {
    public:
        void setRows(juce::StringArray channelNames, juce::StringArray deviceNames);
        void paint(juce::Graphics& g) override;

    private:
        static constexpr int kRowH = 34;
        juce::StringArray names;
        juce::StringArray devices;
    };
    SourceRowList sourceList;
    // Paces the Inputs refresh: sends and device names only move on user
    // action, so a few times a second is enough.
    juce::uint32 lastSourceRefreshMs = 0;

    // Far-right touch surface: drives whichever dial was touched last.
    TouchSlideBar touchBar;

    // Makes a knob turned inside the VST's own window the strip's target.
    PluginKnobWatcher knobWatcher;

    // Docked editor
    juce::AudioProcessorEditor* dockedEditor = nullptr;
    PluginHost* dockedHost = nullptr;
    // The plugin's own UI size at scale 1, measured when it was docked. The bay
    // fits the editor to itself from this size (see applyDockedEditorFit), so a
    // plugin that resizes its UI on its own moves this baseline with it.
    juce::Rectangle<int> dockedEditorNaturalSize;
    // Scale currently handed to the plugin. Stays 1 for a plugin that ignores
    // content scaling, so the natural size above is never derived from a size
    // that was not actually scaled.
    float dockedEditorScale = 1.0f;
    // Set while setScaleFactor() runs: the plugin re-sizes itself synchronously
    // there, and that resize must not be read as the plugin choosing a size.
    bool applyingDockedEditorScale = false;
    // Set while layoutDockedEditor() is inside setBounds, so the editor's own
    // resize callbacks cannot recurse back into it.
    bool layingOutDockedEditor = false;
    // A plugin may size itself after being placed; a couple of corrective passes
    // are enough, and more would fight one that insists on its own minimum.
    int dockedEditorLayoutPasses = 0;
    // Native plugin editors are captured on this worker: PrintWindow makes the
    // plugin repaint its whole UI, which stalled the message thread when it ran
    // inside the thumbnail timer.
    nativedisplay::WindowCaptureWorker thumbnailWorker;
    // A software editor can only be rendered on the message thread; pace that
    // by what the previous snapshot actually cost.
    juce::uint32 lastSnapshotMs = 0;
    int lastSnapshotCostMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RackView)
};
