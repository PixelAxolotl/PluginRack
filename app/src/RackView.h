#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "AudioEngine.h"
#include "DeEsserWindow.h"
#include "DynamicsWindow.h"
#include "EQWindow.h"
#include "LimiterWindow.h"
#include "MultibandWindow.h"
#include "InsertHost.h"
#include "InsertMiniView.h"
#include "LevelMeter.h"
#include "NativeThumbnail.h"
#include "PluginKnobWatcher.h"
#include "StripDialog.h"
#include "Theme.h"
#include "TouchSlideBar.h"
#include "ValueEditSlider.h"

// Channel-centric rack view: 4 columns - channel strip | insert chain (EQ + VSTs)
// | docked plugin editor | output routing. Reuses Mixer fader/meter/EqButton/slots.
class RackView final : public juce::Component,
                       public IInsertHost,
                       private juce::Timer {
public:
    explicit RackView(AudioEngine& engine);
    ~RackView() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void visibilityChanged() override;
    void rebuild();
    void mouseDown(const juce::MouseEvent& e) override;

    void setSelectedChannel(int channelId, bool isOutput);
    juce::Component* getFocusedDiagnosticsEffect() { return focusedEffectWindow; }
    // Headless bay check for --screenshot: resolves an insert by id on the
    // current selection. Production UI uses insertActivated via clicks.
    AudioEngine::InsertSlot* findInsertForDiagnostics(std::uint32_t insertId) {
        return findInsert(insertId);
    }

    // Releases the bay status before the engine destroys the strip it
    // describes (scene restore). Parked while editors live out-of-process.
    void closeDockedEditor();
    // Releases the bay status only when its insert is gone from the engine.
    // A structure change (adding an insert) must not clear the bay: the
    // status line just re-describes the new strips.
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
    void insertBuiltIn(pluginrack::ChainItem item, int chainIndex) override;
    void eraseBuiltIn(pluginrack::ChainItem item) override;
    bool insertActivated(AudioEngine::InsertSlot* insert) override;

private:
    AudioEngine::Channel* currentChannel();
    AudioEngine::Output* currentOutput();
    const std::vector<std::unique_ptr<AudioEngine::InsertSlot>>* currentInserts() const;
    const std::vector<pluginrack::ChainItem>* currentChain() const;
    const std::vector<pluginrack::DynamicsEffect>* currentEffects() const;
    const pluginrack::DynamicsEffect* effectOfType(pluginrack::DynamicsEffectType type) const;
    AudioEngine::InsertSlot* findInsert(std::uint32_t insertId);
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
    void layoutDockedEditor();
    // Re-asserts the HWND holder bounds (JUCE moves the native window to
    // follow it). No-op when nothing is embedded.
    void positionDockedHwnd();
    // Slow watchdog (see timerCallback): re-asserts the fitted frame when
    // the plugin resizes itself back after the fit scale, and re-baselines
    // the natural size when the plugin insists, so a fixed-size plugin ends
    // centred at its real size instead of fighting the bay forever.
    void watchdogDockedEditor();
    // Embeds a host editor HWND into the bay (holder + fit). No-op when
    // the bay has no native peer yet; refreshEditor retries on next layout.
    // fixedSize centres the plugin at its real size without scaling (GH
    // rescaled check for plugins without content-scale support).
    void embedDockedHwnd(unsigned long long hwndValue, int width, int height);
    void embedDockedHwnd(unsigned long long hwndValue, int width, int height, bool fixedSize);
    // Returns the embedded window to the host (hide + detach). Safe when
    // nothing is embedded or the host is gone.
    void releaseDockedHwnd();
    void applyDockedEditorFit(const juce::Rectangle<int>& bay);
    juce::Rectangle<int> dockedEditorVisualBounds() const;
    bool dockedHostAlive() const;
    // Parked while editors live out-of-process (bay shows a status line).
    void hideDockedEditor();
    void showDockedEditor();
    void jumpToMixer();

    void rebuildChannelCombo();
    void rebuildInputCombos(int count);
    void refreshInputControls();
    void applyInputMapping();
    void focusEq();
    void closeEqWindow();
    void focusMultiband();
    void closeEffectWindows();
    void focusDeEsser();
    void focusLimiter();
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
    AudioEngine::AnalyzerTap* currentPluginTap(pluginrack::ChainItemType type);
    void releaseMiniTap();
    void applyVstThumbnail(std::uint32_t insertId, const juce::Image& thumbnail);
    // Rotate mode: snapshots one background insert per turn into an offscreen
    // holder, then destroys it. Returns false when there was nothing to do.
    bool captureNextBackgroundPreview(int targetWidth);
    void clearBackgroundPreview();
    // All-live mode: one hidden editor per insert, refreshed round-robin at a
    // fraction of the focused rate. Editors that fail to build stay frozen.
    void updateAllLivePreviews(int targetWidth);
    void clearAllLivePreviews();

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

    // Plugin column: all built-in state now lives in the insert cards, so
    // there are no standalone EQ/Comp/Gate buttons left to keep.
    juce::Label insertsHeader;
    EQWindow* eqWindow = nullptr;
    DynamicsWindow* dynamicsWindow = nullptr;
    MultibandWindow* multibandWindow = nullptr;
    DeEsserWindow* deEsserWindow = nullptr;
    LimiterWindow* limiterWindow = nullptr;
    juce::Component* focusedEffectWindow = nullptr;
    bool eqFocused = false;
    bool dynamicsFocused = false;
    juce::Viewport slotViewport;
    juce::Component slotContent;
    juce::OwnedArray<InsertSlotComponent> slots;
    // Cards that carry a mini view, so the timer can feed them without walking
    // the list. Cleared at the top of refreshSlots(), which rebuilds every row.
    InsertSlotComponent* eqCard = nullptr;
    InsertSlotComponent* multibandCard = nullptr;
    InsertSlotComponent* deEsserCard = nullptr;
    InsertSlotComponent* limiterCard = nullptr;
    MiniSpectrum multibandSpectrum;
    MiniSpectrum deEsserSpectrum;
    float multibandMeterLevelDb = -60.0f;
    float deEsserReductionDb = 0.0f;
    float deEsserDetS1L = 0.0f, deEsserDetS2L = 0.0f, deEsserDetS1R = 0.0f, deEsserDetS2R = 0.0f;
    // Compressor card: the merged Dynamics card when the gate is there too.
    InsertSlotComponent* dynamicsCard = nullptr;
    bool dynamicsCardPaired = false;
    std::map<std::uint32_t, InsertSlotComponent*> vstCards;
    // Last snapshot per insert. Parked (always empty) while editors live
    // out-of-process; kept so the native-reparent follow-up reuses the map.
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
    // Native holder for the embedded host window: positioned exactly like
    // the GH bay positions its editor (holder-local, centred, clamped).
    // JUCE moves the HWND to follow this component (DPI included), so the
    // window hides with the Racks tab and clips to the bay like the GH
    // editor does — no manual SetWindowPos math.
    juce::HWNDComponent dockedHwndView;
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
        void setRows(juce::StringArray channelNames, juce::StringArray deviceNames,
                     const std::vector<float>& channelHues);
        void paint(juce::Graphics& g) override;

    private:
        static constexpr int kRowH = 34;
        juce::StringArray names;
        juce::StringArray devices;
        std::vector<float> hues;
    };
    SourceRowList sourceList;
    // Paces the Inputs refresh: sends and device names only move on user
    // action, so a few times a second is enough.
    juce::uint32 lastSourceRefreshMs = 0;

    // Far-right touch surface: drives whichever dial was touched last.
    // Inert while editors live out-of-process (no in-process instance to
    // observe); kept for the native-reparent follow-up.
    TouchSlideBar touchBar;

    // Makes a knob turned inside the VST's own window the strip's target.
    // Inert while editors live out-of-process; kept for the follow-up.
    PluginKnobWatcher knobWatcher;

    // Docked editor bay: the focused insert's editor lives in the
    // sandboxed host process. The bay embeds the host's native window via
    // dockedHwndView (a JUCE HWND holder, see refreshEditor): dockedHost
    // tracks which insert the bay describes, dockedHwnd is the embedded
    // window (0 when none), and the fit state below scales it into the bay.
    // dockedHwndView is positioned exactly like the GH bay positions its
    // editor; JUCE moves the native window to follow it (DPI included), so
    // the fit math stays holder-local while the Win32 call gets
    // DPI-correct physical pixels.
    juce::AudioProcessorEditor* dockedEditor = nullptr;
    PluginHost* dockedHost = nullptr;
    // Native HWND of the embedded host editor window (as an integer: HWND
    // does not survive a 32/64-bit read cleanly). 0 when nothing is
    // embedded. Owned by the host process — never destroyed here, only
    // reparented back on release.
    unsigned long long dockedHwnd = 0;
    int dockedHwndWidth = 0;
    int dockedHwndHeight = 0;
    juce::Rectangle<int> dockedEditorNaturalSize;
    // Fitted frame size last computed (logical units). Guards the
    // resize round-trip: recompute only when the fit changes, so the bay
    // and a plugin that answers with a slightly different size cannot
    // ping-pong.
    juce::Rectangle<int> dockedEditorAppliedSize;
    // Frame the bay last asked for (holder-local, see layoutDockedEditor).
    // The watchdog compares the live window against this: a resizable
    // plugin settles into it, a fixed-size one snaps back and is centred
    // at its real size from then on.
    juce::Rectangle<int> dockedEditorWant;
    int dockedBayW = 0;
    int dockedBayH = 0;
    bool dockedEditorSettled = false;
    int dockedWatchdogTick = 0;
    float dockedEditorScale = 1.0f;
    // Fixed-size plugin: the host reported it cannot stretch, so the bay
    // centres it at its real size without sending any scale (GH's rescaled
    // check — a scale the plugin ignores must read as scale 1.0).
    bool dockedEditorFixedSize = false;
    bool applyingDockedEditorScale = false;
    bool layingOutDockedEditor = false;
    int dockedEditorLayoutPasses = 0;
    // Native plugin editors are captured on this worker: PrintWindow makes the
    // plugin repaint its whole UI, which stalled the message thread when it ran
    // inside the thumbnail timer. Parked until the bay embeds host windows.
    nativedisplay::WindowCaptureWorker thumbnailWorker;
    // Rotate mode: parked until the bay embeds host windows. Kept (always
    // null) so the preview-mode plumbing keeps compiling.
    juce::AudioProcessorEditor* backgroundEditor = nullptr;
    PluginHost* backgroundHost = nullptr;
    std::uint32_t backgroundInsertId = 0;
    // All-live mode: parked until the bay embeds host windows. Kept (always
    // empty) so the preview-mode plumbing keeps compiling.
    struct LivePreview {
        juce::AudioProcessorEditor* editor = nullptr;
        PluginHost* host = nullptr;
    };
    std::map<std::uint32_t, LivePreview> livePreviews;
    std::set<std::uint32_t> livePreviewFailed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RackView)
};
