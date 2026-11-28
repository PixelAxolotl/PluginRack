#include "IoView.h"

#include "DeviceNames.h"

namespace {
constexpr int kTabWidth = 96;
// Fixed list while the device width is unknown; covers stereo to 7/8.
constexpr int kUnknownDevicePairs = 4;
constexpr int kPairComboWidth = 86;
constexpr int kPairRowWidth = 150;
}

IoView::IoView(AudioEngine& engine)
    : audioEngine(engine)
    , inputsBoard(engine)
    , outputsBoard(engine)
    , busesBoard(engine) {
    addAndMakeVisible(header);

    const auto styleTab = [this](juce::TextButton& tab, const juce::String& tooltip) {
        tab.setClickingTogglesState(true);
        theme::ui::styleButton(tab);
        tab.setTooltip(tooltip);
        addAndMakeVisible(tab);
    };
    styleTab(inputsTab, "Show the input channel strips");
    styleTab(outputsTab, "Show the output strips");
    styleTab(busesTab, "Show the bus strips");
    inputsTab.onClick = [this] { selectTab(Tab::Inputs); };
    outputsTab.onClick = [this] { selectTab(Tab::Outputs); };
    busesTab.onClick = [this] { selectTab(Tab::Buses); };

    inputsBoard.setSections(MixerComponent::Sections::InputsOnly);
    outputsBoard.setSections(MixerComponent::Sections::OutputsOnly);
    busesBoard.setSections(MixerComponent::Sections::BusesOnly);
    for (auto* board : { &inputsBoard, &outputsBoard, &busesBoard }) {
        board->onStructureChanged = [this] {
            if (onStructureChanged) onStructureChanged();
        };
        board->onDoubleClicked = [this](int id, bool isOut) {
            if (onDoubleClicked) onDoubleClicked(id, isOut);
        };
    }
    inputsBoard.onSelectionChanged = [this](int id, bool isOut) {
        if (onSelectionChanged) onSelectionChanged(id, isOut);
    };
    busesBoard.onSelectionChanged = [this](int id, bool isOut) {
        if (onSelectionChanged) onSelectionChanged(id, isOut);
    };
    // The picker acts on the selected output strip; other boards keep the
    // plain forward. Buses never reach here (own board), and a stale id
    // drops out in updatePairCombo through the empty device state.
    outputsBoard.onSelectionChanged = [this](int id, bool isOut) {
        if (isOut && id > 0) selectedOutputId = static_cast<std::uint32_t>(id);
        updatePairCombo();
        if (onSelectionChanged) onSelectionChanged(id, isOut);
    };
    addAndMakeVisible(inputsBoard);
    addAndMakeVisible(outputsBoard);
    addAndMakeVisible(busesBoard);

    // Same control styling as the strips' mode combo.
    pairLabel.setColour(juce::Label::textColourId, theme::text());
    pairLabel.setJustificationType(juce::Justification::centredRight);
    pairCombo.setColour(juce::ComboBox::backgroundColourId, theme::control());
    pairCombo.setColour(juce::ComboBox::textColourId, theme::text());
    pairCombo.setColour(juce::ComboBox::arrowColourId, theme::dim());
    pairCombo.setColour(juce::ComboBox::outlineColourId, theme::border());
    pairCombo.onChange = [this] {
        const int id = pairCombo.getSelectedId();
        if (id <= 0 || selectedOutputId == 0) return;
        audioEngine.setOutputFirstChannel(selectedOutputId, id - 1);
    };
    addAndMakeVisible(pairLabel);
    addAndMakeVisible(pairCombo);
    selectTab(Tab::Inputs);

    startTimer(250);
}

IoView::~IoView() { stopTimer(); }

void IoView::selectTab(Tab tab) {
    currentTab = tab;
    inputsTab.setToggleState(tab == Tab::Inputs, juce::dontSendNotification);
    outputsTab.setToggleState(tab == Tab::Outputs, juce::dontSendNotification);
    busesTab.setToggleState(tab == Tab::Buses, juce::dontSendNotification);
    inputsBoard.setVisible(tab == Tab::Inputs);
    outputsBoard.setVisible(tab == Tab::Outputs);
    busesBoard.setVisible(tab == Tab::Buses);
    if (tab == Tab::Outputs) updatePairCombo();
    resized();
}

void IoView::rebuild() {
    inputsBoard.rebuild();
    outputsBoard.rebuild();
    busesBoard.rebuild();
    updatePairCombo();
    resized();
    repaint();
}

void IoView::refreshDeviceStrips() {
    inputsBoard.refreshStrips();
    outputsBoard.refreshStrips();
    busesBoard.refreshStrips();
    updatePairCombo();
}

void IoView::refresh() {
    const auto report = audioEngine.getLatencyReport();
    if (report.valid) {
        const auto khz = report.sampleRate / 1000.0;
        header.setStatus(juce::String(khz, khz < 10.0 ? 1 : 0) + " kHz  -  "
                         + juce::String(report.inputBufferSize) + " in / "
                         + juce::String(report.outputBufferSize) + " out");
    } else {
        header.setStatus("No clock output", theme::amber());
    }
}

void IoView::resized() {
    auto area = getLocalBounds().reduced(theme::ui::kPagePad);
    header.setBounds(area.removeFromTop(theme::ui::kHeaderH));
    area.removeFromTop(theme::ui::kHeaderGap);

    auto tabs = area.removeFromTop(theme::ui::kControlH);
    inputsTab.setBounds(tabs.removeFromLeft(kTabWidth));
    tabs.removeFromLeft(theme::ui::kRowGap);
    outputsTab.setBounds(tabs.removeFromLeft(kTabWidth));
    tabs.removeFromLeft(theme::ui::kRowGap);
    busesTab.setBounds(tabs.removeFromLeft(kTabWidth));
    area.removeFromTop(theme::ui::kHeaderGap);

    // Compact pair picker, right-aligned in the tab row on the Outputs tab.
    const bool showPair = currentTab == Tab::Outputs;
    pairLabel.setVisible(showPair);
    pairCombo.setVisible(showPair);
    if (showPair && tabs.getWidth() >= kPairRowWidth) {
        auto pairArea = tabs.removeFromRight(kPairRowWidth);
        pairCombo.setBounds(pairArea.removeFromRight(kPairComboWidth));
        pairArea.removeFromRight(theme::ui::kRowGap);
        pairLabel.setBounds(pairArea);
    }

    inputsBoard.setBounds(area);
    outputsBoard.setBounds(area);
    busesBoard.setBounds(area);
}

void IoView::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
}

void IoView::timerCallback() {
    refresh();
    // A reopened device reports its width here; skip while the menu is open
    // so a rebuild cannot pull the list out from under the user.
    if (currentTab == Tab::Outputs && !pairCombo.isPopupActive()) updatePairCombo();
}

void IoView::updatePairCombo() {
    // Buses resolve through the strip lookup but carry no device, so they
    // drop out with the silent case below and the picker stays disabled.
    const auto state = audioEngine.getOutputDeviceState(selectedOutputId);
    const bool hasDevice = selectedOutputId != 0 && state.deviceName.isNotEmpty() &&
                           !isSilentDeviceName(state.deviceName);
    const int deviceChans =
        hasDevice ? audioEngine.getOutputDeviceChannelCount(selectedOutputId) : 0;
    const int channels = deviceChans > 0 ? deviceChans : kUnknownDevicePairs * 2;
    pairCombo.clear(juce::dontSendNotification);
    for (int i = 0; i < channels; ++i) {
        // Any channel can start the pair: a device numbered 1, 2, 3, ... can
        // host 3/4, and the engine opens enough channels for it.
        pairCombo.addItem("From " + juce::String(i + 1), i + 1);
    }
    const bool usable = hasDevice && channels > 0;
    pairLabel.setEnabled(usable);
    pairCombo.setEnabled(usable);
    if (!usable) {
        pairCombo.setTooltip("Select an output with a device to pick its start channel");
        return;
    }
    juce::String outputName;
    for (const auto& o : audioEngine.getOutputs()) {
        if (o != nullptr && o->id == selectedOutputId) {
            outputName = o->name;
            break;
        }
    }
    pairCombo.setTooltip("Device start channel for \"" + outputName + "\"");
    // The device may open narrower than the saved pair; the engine falls back
    // to 1/2 there, so the picker shows the effective start channel.
    const int pair = audioEngine.getOutputFirstChannel(selectedOutputId);
    pairCombo.setSelectedId(juce::jmin(pair + 1, channels), juce::dontSendNotification);
}
