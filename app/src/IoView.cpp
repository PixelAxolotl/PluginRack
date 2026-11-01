#include "IoView.h"

namespace {
constexpr int kTabWidth = 96;
}

IoView::IoView(AudioEngine& engine)
    : audioEngine(engine)
    , inputsBoard(engine)
    , outputsBoard(engine) {
    addAndMakeVisible(header);

    const auto styleTab = [this](juce::TextButton& tab, const juce::String& tooltip) {
        tab.setClickingTogglesState(true);
        theme::ui::styleButton(tab);
        tab.setTooltip(tooltip);
        addAndMakeVisible(tab);
    };
    styleTab(inputsTab, "Show the input channel strips");
    styleTab(outputsTab, "Show the output strips");
    inputsTab.onClick = [this] { selectTab(true); };
    outputsTab.onClick = [this] { selectTab(false); };

    inputsBoard.setSections(MixerComponent::Sections::InputsOnly);
    outputsBoard.setSections(MixerComponent::Sections::OutputsOnly);
    inputsBoard.onStructureChanged = [this] {
        if (onStructureChanged) onStructureChanged();
    };
    outputsBoard.onStructureChanged = [this] {
        if (onStructureChanged) onStructureChanged();
    };
    addAndMakeVisible(inputsBoard);
    addAndMakeVisible(outputsBoard);
    selectTab(true);

    startTimer(250);
}

IoView::~IoView() { stopTimer(); }

void IoView::selectTab(bool showInputTab) {
    showInputs = showInputTab;
    inputsTab.setToggleState(showInputs, juce::dontSendNotification);
    outputsTab.setToggleState(!showInputs, juce::dontSendNotification);
    inputsBoard.setVisible(showInputs);
    outputsBoard.setVisible(!showInputs);
    resized();
}

void IoView::rebuild() {
    inputsBoard.rebuild();
    outputsBoard.rebuild();
    resized();
    repaint();
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
    area.removeFromTop(theme::ui::kHeaderGap);

    inputsBoard.setBounds(area);
    outputsBoard.setBounds(area);
}

void IoView::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
}

void IoView::timerCallback() { refresh(); }
