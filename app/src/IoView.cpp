#include "IoView.h"

namespace {
constexpr int kTabWidth = 96;
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
        board->onSelectionChanged = [this](int id, bool isOut) {
            if (onSelectionChanged) onSelectionChanged(id, isOut);
        };
        board->onDoubleClicked = [this](int id, bool isOut) {
            if (onDoubleClicked) onDoubleClicked(id, isOut);
        };
    }
    addAndMakeVisible(inputsBoard);
    addAndMakeVisible(outputsBoard);
    addAndMakeVisible(busesBoard);
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
    resized();
}

void IoView::rebuild() {
    inputsBoard.rebuild();
    outputsBoard.rebuild();
    busesBoard.rebuild();
    resized();
    repaint();
}

void IoView::refreshDeviceStrips() {
    inputsBoard.refreshStrips();
    outputsBoard.refreshStrips();
    busesBoard.refreshStrips();
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

    inputsBoard.setBounds(area);
    outputsBoard.setBounds(area);
    busesBoard.setBounds(area);
}

void IoView::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
}

void IoView::timerCallback() { refresh(); }
