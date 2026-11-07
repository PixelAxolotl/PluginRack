#include "LimiterWindow.h"
#include <algorithm>
#include <cmath>
using namespace pluginrack;

LimiterWindow::LimiterWindow(const juce::String& title, const LimiterEffect& initialEffect, ApplyCallback applyCallback)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      effect(initialEffect), apply(std::move(applyCallback)),
      historyInput(240, -60.0f), historyReduction(240, 0.0f), historyOutput(240, -60.0f) {
    setResizable(false, false); setContentOwned(&content, false); setUsingNativeTitleBar(true);
    centreWithSize(kWidth, kHeight);
    modeLabel.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    modeLabel.setColour(juce::Label::textColourId, theme::cyan());
    content.addAndMakeVisible(modeLabel); content.addAndMakeVisible(graph); content.addAndMakeVisible(enableButton);
    enableButton.setClickingTogglesState(true); enableButton.setToggleState(!effect.bypassed, juce::dontSendNotification);
    // Match the Multiband header-button treatment: themed face and text.
    theme::ui::styleButton(enableButton);
    theme::ui::styleCombo(presetCombo);
    enableButton.onClick = [this] { effect.bypassed = !enableButton.getToggleState(); if (apply) apply(effect, true); };
    content.addAndMakeVisible(presetCombo);
    presetCombo.addItem("Custom", 1); presetCombo.addItem("Default", 2); presetCombo.addItem("Safe", 3); presetCombo.addItem("Loud", 4);
    presetCombo.setSelectedId(2, juce::dontSendNotification);
    presetCombo.onChange = [this] { applyPreset(presetCombo.getSelectedId()); };
    content.addAndMakeVisible(inputGain); content.addAndMakeVisible(inputGainLabel);
    content.addAndMakeVisible(ceiling); content.addAndMakeVisible(ceilingLabel);
    content.addAndMakeVisible(lookahead); content.addAndMakeVisible(lookaheadLabel);
    content.addAndMakeVisible(release); content.addAndMakeVisible(releaseLabel);
    // Use the app's default EQ/Dynamics rotary-knob treatment.
    const auto styleRotary = [](ValueEditSlider& slider, double min, double max, double step,
                                const juce::String& suffix, double resetValue, juce::Colour fill) {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 18);
        slider.setRange(min, max, step);
        slider.setTextValueSuffix(suffix);
        slider.setDoubleClickReturnValue(true, resetValue);
        slider.setColour(juce::Slider::rotarySliderFillColourId, fill);
        slider.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
        slider.setColour(juce::Slider::textBoxTextColourId, theme::text());
        slider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
        slider.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    };
    styleRotary(inputGain, -48.0, 48.0, 0.1, " dB", 0.0, theme::cyan());
    styleRotary(ceiling, -12.0, 0.0, 0.1, " dB", -1.0, theme::green());
    styleRotary(lookahead, 0.0, 10.0, 0.1, " ms", 3.0, theme::amber());
    styleRotary(release, 1.0, 2000.0, 0.1, " ms", 100.0, theme::violet());
    for (auto* label : {&inputGainLabel, &ceilingLabel, &lookaheadLabel, &releaseLabel}) {
        label->setJustificationType(juce::Justification::centred);
        label->setFont(juce::Font(juce::FontOptions(11.0f)));
        label->setColour(juce::Label::textColourId, theme::dim());
    }
    inputGain.onValueChange = [this] { effect.inputGainDb = (float)inputGain.getValue(); apply(effect, false); };
    ceiling.onValueChange = [this] { effect.ceilingDb = (float)ceiling.getValue(); apply(effect, false); };
    lookahead.onValueChange = [this] { effect.lookaheadMs = (float)lookahead.getValue(); apply(effect, false); };
    release.onValueChange = [this] { effect.releaseMs = (float)release.getValue(); apply(effect, false); };
    graph.setMode(TraceGraphMode::Time);
    graph.setOverlayPainter([this](juce::Graphics& g, juce::Rectangle<float> graphBounds) {
        const auto ceilingY = graphBounds.getBottom()
            - std::clamp((effect.ceilingDb + 60.0f) / 72.0f, 0.0f, 1.0f) * graphBounds.getHeight();
        g.setColour(theme::cyan().withAlpha(0.75f));
        g.drawHorizontalLine(juce::roundToInt(ceilingY), graphBounds.getX(), graphBounds.getRight());
    });
    syncControls();
    setVisible(true);
    startTimer(33);
}

void LimiterWindow::syncControls() {
    inputGain.setValue(effect.inputGainDb, juce::dontSendNotification);
    ceiling.setValue(effect.ceilingDb, juce::dontSendNotification);
    lookahead.setValue(effect.lookaheadMs, juce::dontSendNotification);
    release.setValue(effect.releaseMs, juce::dontSendNotification);
}

void LimiterWindow::applyPreset(int id) {
    if (id == 2) effect = makeDefaultLimiterEffect();
    if (id == 3) { effect.ceilingDb = -1.0f; effect.inputGainDb = 0; effect.lookaheadMs = 3.0f; }
    if (id == 4) { effect.ceilingDb = -0.1f; effect.inputGainDb = 6.0f; }
    syncControls(); apply(effect, false);
}

void LimiterWindow::setBypassed(bool b) { effect.bypassed = b; enableButton.setToggleState(!b, juce::dontSendNotification); }
void LimiterWindow::setAnalyzerMode(int m) { if (m == 0) stopTimer(); else startTimer(33); }

void LimiterWindow::timerCallback() {
    if (!pullAnalyzerTap) return;
    pullAnalyzerTap(false, preTapL.data(), preTapR.data(), static_cast<int>(preTapL.size()));
    pullAnalyzerTap(true, postTapL.data(), postTapR.data(), static_cast<int>(postTapL.size()));

    const auto measurePeakDb = [](const auto& left, const auto& right) {
        float peak = 0.0f;
        for (std::size_t i = 0; i < left.size(); ++i)
            peak = std::max(peak, std::max(std::fabs(left[i]), std::fabs(right[i])));
        return 20.0f * std::log10(std::max(peak, 1.0e-8f));
    };
    const float inputDb = measurePeakDb(preTapL, preTapR) + effect.inputGainDb;
    const float outputDb = measurePeakDb(postTapL, postTapR);
    const float reductionDb = std::max(0.0f, inputDb - outputDb);
    levelHistory.push(inputDb, reductionDb, outputDb);
    levelHistory.copyOrderedTo(std::span<float>(historyInput),
                               std::span<float>(historyReduction),
                               std::span<float>(historyOutput));
    const auto count = levelHistory.size();
    graph.setTimeData(TimeTraceData{
        {historyInput.begin(), historyInput.begin() + static_cast<std::ptrdiff_t>(count)},
        {historyReduction.begin(), historyReduction.begin() + static_cast<std::ptrdiff_t>(count)},
        {historyOutput.begin(), historyOutput.begin() + static_cast<std::ptrdiff_t>(count)}});
    peakDb = outputDb;
    grDb = reductionDb;
    graph.repaint();
}

void LimiterWindow::resized() {
    auto* installedContent = getContentComponent();
    if (installedContent == nullptr) return;
    installedContent->setBounds(getLocalBounds());
    auto area = installedContent->getLocalBounds().reduced(8);
    auto header = area.removeFromTop(28);
    modeLabel.setBounds(header.removeFromLeft(250));
    presetCombo.setBounds(header.removeFromRight(120));
    header.removeFromRight(6);
    enableButton.setBounds(header.removeFromRight(72));
    area.removeFromTop(6);
    auto controls = area.removeFromBottom(132);
    area.removeFromBottom(8);
    graph.setBounds(area);
    const int knobWidth = juce::jmax(70, controls.getWidth() / 4);
    auto placeKnob = [&](ValueEditSlider& slider, juce::Label& label) {
        auto slot = controls.removeFromLeft(knobWidth);
        label.setBounds(slot.removeFromTop(16));
        slider.setBounds(slot.reduced(2, 0));
    };
    placeKnob(inputGain, inputGainLabel);
    placeKnob(ceiling, ceilingLabel);
    placeKnob(lookahead, lookaheadLabel);
    placeKnob(release, releaseLabel);
}
