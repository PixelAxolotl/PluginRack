#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "ChannelStrip.h"
#include "DeviceControl.h"
#include "EQWindow.h"
#include "DynamicsWindow.h"
#include "InsertHost.h"
#include "DeviceMenu.h"
#include "LevelMeter.h"
#include "MiniButton.h"
#include "Theme.h"
#include "ValueEditSlider.h"

class MixerComponent final : public juce::Component {
public:
    // Which strip columns this board shows. The Mixer shows both; the I/O page
    // hosts one board per side so each side gets its own tab.
    enum class Sections { Both, InputsOnly, OutputsOnly };

    explicit MixerComponent(AudioEngine& engine);

    void setSections(Sections sections);
    Sections getSections() const { return sections_; }

    void resized() override;
    void rebuild();
    // Re-reads the engine's device assignments (and their errors) into the
    // existing strips without rebuilding them. Used when a background device
    // open finishes, so a device that no longer exists turns red immediately.
    void refreshStrips();

    // Closes every open channel/output EQ window synchronously. Call this
    // while the engine strips are still alive (before any removeChannel /
    // removeOutput): the strips hold Channel&/Output& refs and their EQ
    // windows call back into those refs on destruction (analyzerTap), so
    // destroying the engine strips first and rebuilding after would leave
    // the teardown reading freed channels/outputs.
    void closeAllEqWindows();

    std::function<void()> onStructureChanged;
    std::function<void(int channelId, bool isOutput)> onSelectionChanged;
    std::function<void(int channelId, bool isOutput)> onDoubleClicked;

private:
    class OutputStrip final : public juce::Component, public IInsertHost {
    public:
        // Output EQ is keyed by scene-order position (Task 3 asymmetry), so the
        // strip captures its index at construction; rebuild() recreates strips
        // whenever the structure changes, keeping it fresh.
        OutputStrip(AudioEngine::Output& output, int outputIndex, AudioEngine& engine);
        ~OutputStrip() override;
        void resized() override;
        void paint(juce::Graphics& g) override;
        void refresh();
        void openEqWindow();
        void closeEqWindow();
        void refreshEqButton();
        void openDynamicsWindow(pluginrack::DynamicsEffectType type);
        void toggleDynamicsBypass(pluginrack::DynamicsEffectType type);
        void refreshDynamicsButtons();
        int getOutputIndex() const { return outputIndex; }

        // IInsertHost
        juce::String insertPlugin(const juce::File&, int pluginUniqueId = 0) override;
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

        std::function<void()> onRemoveRequested;
        std::function<void()> onRenamed;
        std::function<void()> onSelected;
        std::function<void()> onDoubleClicked;

        void mouseDown(const juce::MouseEvent&) override;
        void mouseDoubleClick(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseMove(const juce::MouseEvent&) override;

        // Output reordering: the top-left grip reports the drag in screen space
        // and the board maps it to an insertion index, exactly like the input
        // strips' handle.
        void setDragHandleVisible(bool visible);
        std::function<void()> onDragHandleStarted;
        std::function<void(juce::Point<int> screenPosition)> onDragHandleMoved;
        std::function<void()> onDragHandleFinished;

    private:
        void refreshInserts();
        int chainBoundaryY(int targetIndex) const;
        void paintDragHandle(juce::Graphics& g) const;

        juce::Rectangle<int> dragHandleBounds;
        bool dragHandleVisible = false;
        bool draggingHandle = false;

        AudioEngine::Output& output;
        int outputIndex = 0;
        AudioEngine& audioEngine;
        DeviceControl deviceControl;
        MiniButton removeOutputButton{juce::String::fromUTF8("\xc3\x97")};
        juce::ComboBox modeCombo;
        MiniButton flipButton{"Flip"};
        MiniButton streamButton{"Strm"};
        juce::OwnedArray<InsertSlotComponent> slots;
        int chainDropTarget = -1;
        int chainRowCount = 0;
        juce::Label vstHeader{"", "VST RACK"};
        ValueEditSlider gainSlider;
        LevelMeter meter;
        juce::TextButton muteButton{"Mute"};
        EqButton gateButton{"GATE"};
        EqButton eqButton{"EQ"};
        EqButton compressorButton{"COMP"};
        EQWindow* eqWindow = nullptr;
        DynamicsWindow* dynamicsWindow = nullptr;
    };

    void rebuildChannelStrips();
    void rebuildOutputStrips();
    void layoutChannelStrips(juce::Rectangle<int> viewportBounds);
    void layoutOutputStrips(juce::Rectangle<int> panelBounds);
    void updateChannelDrag(juce::Point<int> screenPosition);
    void finishChannelDrag(std::uint32_t channelId, int sourceIndex);
    void updateOutputDrag(juce::Point<int> screenPosition);
    void finishOutputDrag(std::uint32_t outputId, int sourceIndex);
    // Boundary index (0..count) in the pre-move list -> the index the engine
    // inserts at after removal. Dropping to the right of the dragged strip's
    // own slot shifts by one.
    static int moveTargetIndex(int insertionIndex, int sourceIndex, int count);
    // X of the light-blue insertion bar for a boundary, always inside the
    // gutter so the neighbouring strips cannot paint over it.
    static float insertionBarX(int insertionIndex, int inset, int step, int gap);

    // Hosts the strips and paints the light-blue insertion bar while one is
    // dragged by its handle (same marker vocabulary as the VST chain drag).
    class StripLane final : public juce::Component {
    public:
        void paint(juce::Graphics& g) override;
        float dropMarkerX = -1.0f;
    };

    AudioEngine& audioEngine;
    Sections sections_ = Sections::Both;
    juce::TextButton addChannelButton{"+ Ch"};
    juce::TextButton addOutputButton{"+ Out"};
    juce::Label inputHeader{"", "INPUTS"};
    juce::Viewport channelViewport;
    StripLane stripContent;
    int channelInsertIndex = -1;
    juce::OwnedArray<ChannelStrip> strips;

    juce::Component masterPanel;
    juce::Label masterHeader{"", "OUTPUTS"};
    juce::Viewport masterViewport;
    StripLane masterContent;
    int outputInsertIndex = -1;
    juce::OwnedArray<OutputStrip> outputStrips;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerComponent)
};