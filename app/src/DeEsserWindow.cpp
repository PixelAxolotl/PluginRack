#include "DeEsserWindow.h"
#include <algorithm>
#include <cmath>
using namespace pluginrack;

DeEsserWindow::DeEsserWindow(const juce::String& title, const DeEsserEffect& initialEffect,
                             ApplyCallback applyCallback,
                             float initialRtaOffsetDb, int initialWeighting,
                             double initialSampleRateHz)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      effect(initialEffect), apply(std::move(applyCallback)),
      sampleRateHz(initialSampleRateHz > 0.0 ? initialSampleRateHz : 48000.0),
      analyzerFft(std::make_unique<juce::dsp::FFT>(13)) {
    setResizable(false, false); setContentOwned(&content, false); setUsingNativeTitleBar(true);
    centreWithSize(kWidth, kHeight);

    hannCoeffs.resize(static_cast<std::size_t>(kAnalyzerFftSize));
    for (int i = 0; i < kAnalyzerFftSize; ++i) {
        const double t = juce::MathConstants<double>::twoPi * i / kAnalyzerFftSize;
        hannCoeffs[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(t));
    }
    fftScratch.assign(static_cast<std::size_t>(kAnalyzerFftSize) * 2, 0.0f);
    tapL.assign(static_cast<std::size_t>(kAnalyzerFftSize), 0.0f);
    tapR.assign(static_cast<std::size_t>(kAnalyzerFftSize), 0.0f);
    spectrumDb.assign(static_cast<std::size_t>(kAnalyzerFftSize) / 2 + 1, 0.0f);
    spectrumAvg.assign(static_cast<std::size_t>(kAnalyzerFftSize) / 2 + 1, -100.0f);
    modeLabel.setText("DE-ESSER", juce::dontSendNotification);
    theme::ui::styleTitleLabel(modeLabel);
    content.addAndMakeVisible(modeLabel);
    content.addAndMakeVisible(analyzerLabel);
    content.addAndMakeVisible(rtaMinusButton);
    content.addAndMakeVisible(rtaOffsetLabel);
    content.addAndMakeVisible(rtaPlusButton);
    content.addAndMakeVisible(weightingZ);
    content.addAndMakeVisible(weightingA);
    content.addAndMakeVisible(weightingC);
    content.addAndMakeVisible(graph); content.addAndMakeVisible(enableButton);
    enableButton.setClickingTogglesState(true); enableButton.setToggleState(!effect.bypassed, juce::dontSendNotification);
    // Match the Multiband header-button treatment: themed face and text.
    theme::ui::styleButton(enableButton);
    theme::ui::styleButton(modeButton);
    theme::ui::styleCombo(presetCombo);
    enableButton.onClick = [this] { effect.bypassed = !enableButton.getToggleState(); if (apply) apply(effect, true); };
    content.addAndMakeVisible(modeButton);
    modeButton.onClick = [this] {
        effect.mode = effect.mode == DeEsserMode::Split ? DeEsserMode::Wideband : DeEsserMode::Split;
        modeButton.setButtonText(effect.mode == DeEsserMode::Split ? "Split" : "Wideband"); apply(effect, false);
    };
    content.addAndMakeVisible(presetCombo);
    presetCombo.addItem("Custom", 1); presetCombo.addItem("Default", 2); presetCombo.addItem("Gentle", 3); presetCombo.addItem("Harsh", 4);
    presetCombo.setSelectedId(2, juce::dontSendNotification);
    presetCombo.onChange = [this] { applyPreset(presetCombo.getSelectedId()); };

    analyzerLabel.setJustificationType(juce::Justification::centredRight);
    analyzerLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    analyzerLabel.setColour(juce::Label::textColourId, theme::dim());
    rtaMinusButton.setTooltip("RTA display: shift the analyzer down");
    rtaMinusButton.setFontSize(9.0f);
    rtaMinusButton.setClickAction([this] { setRtaOffsetDb(rtaOffsetDb - kTraceRtaStepDb); });
    rtaPlusButton.setTooltip("RTA display: shift the analyzer up");
    rtaPlusButton.setFontSize(9.0f);
    rtaPlusButton.setClickAction([this] { setRtaOffsetDb(rtaOffsetDb + kTraceRtaStepDb); });
    rtaOffsetLabel.setJustificationType(juce::Justification::centred);
    rtaOffsetLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    rtaOffsetLabel.setColour(juce::Label::textColourId, theme::dim());
    rtaOffsetLabel.setTooltip("RTA display offset in dB, saved with the scene");
    const char* weightingTips[3] = {
        "Z: flat - the spectrum as measured", "A: A-weighting (IEC 61672)", "C: C-weighting (IEC 61672)"};
    for (int i = 0; i < 3; ++i) {
        weightingButtons[static_cast<std::size_t>(i)]->setTooltip(weightingTips[i]);
        weightingButtons[static_cast<std::size_t>(i)]->setFontSize(9.0f);
        weightingButtons[static_cast<std::size_t>(i)]->setClickAction(
            [this, i] { setWeightingIndex(i); });
    }
    setWeightingIndex(initialWeighting);
    setRtaOffsetDb(initialRtaOffsetDb);

    content.addAndMakeVisible(freq); content.addAndMakeVisible(freqLabel);
    content.addAndMakeVisible(q); content.addAndMakeVisible(qLabel);
    content.addAndMakeVisible(threshold); content.addAndMakeVisible(thresholdLabel);
    content.addAndMakeVisible(range); content.addAndMakeVisible(rangeLabel);
    content.addAndMakeVisible(attack); content.addAndMakeVisible(attackLabel);
    content.addAndMakeVisible(release); content.addAndMakeVisible(releaseLabel);

    // Match the app's default EQ/Dynamics rotary-slider treatment.
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
    styleRotary(freq, 100.0, 16000.0, 1.0, " Hz", 6000.0, theme::cyan());
    styleRotary(q, 0.1, 20.0, 0.01, "", 2.0, theme::violet());
    styleRotary(threshold, -60.0, 0.0, 0.1, " dB", -30.0, theme::cyan());
    styleRotary(range, 0.0, 36.0, 0.1, " dB", 12.0, theme::green());
    styleRotary(attack, 0.01, 200.0, 0.1, " ms", 2.0, theme::amber());
    styleRotary(release, 1.0, 1000.0, 0.1, " ms", 100.0, theme::violet());
    for (auto* label : {&freqLabel, &qLabel, &thresholdLabel, &rangeLabel, &attackLabel, &releaseLabel}) {
        label->setJustificationType(juce::Justification::centred);
        label->setFont(juce::Font(juce::FontOptions(11.0f)));
        label->setColour(juce::Label::textColourId, theme::dim());
    }
    freq.onValueChange = [this] { effect.freqHz = (float)freq.getValue(); apply(effect, false); };
    q.onValueChange = [this] { effect.q = (float)q.getValue(); apply(effect, false); };
    threshold.onValueChange = [this] { effect.thresholdDb = (float)threshold.getValue(); apply(effect, false); };
    range.onValueChange = [this] { effect.rangeDb = (float)range.getValue(); apply(effect, false); };
    attack.onValueChange = [this] { effect.attackMs = (float)attack.getValue(); apply(effect, false); };
    release.onValueChange = [this] { effect.releaseMs = (float)release.getValue(); apply(effect, false); };
    graph.setMode(TraceGraphMode::Frequency);
    graph.setOverlayPainter([this](juce::Graphics& g, juce::Rectangle<float> graphBounds) {
        const auto freqToX = [graphBounds](float frequency) {
            const auto position = std::log(std::clamp(static_cast<double>(frequency), 20.0, 20000.0) / 20.0)
                / std::log(1000.0);
            return graphBounds.getX() + static_cast<float>(position) * graphBounds.getWidth();
        };
        const auto thresholdY = graphBounds.getBottom()
            - std::clamp((effect.thresholdDb + 60.0f) / 72.0f, 0.0f, 1.0f) * graphBounds.getHeight();
        g.setColour(theme::cyan().withAlpha(0.65f));
        g.drawHorizontalLine(juce::roundToInt(thresholdY), graphBounds.getX(), graphBounds.getRight());
        g.setColour(theme::text().withAlpha(0.8f));
        g.drawVerticalLine(juce::roundToInt(freqToX(effect.freqHz)), graphBounds.getY(), graphBounds.getBottom());
    });
    syncControls();
    setVisible(true);
    startTimer(33);
}

void DeEsserWindow::setRtaOffsetDb(float offsetDb) {
    const float clamped = juce::jlimit(kTraceRtaMinOffsetDb, kTraceRtaMaxOffsetDb, offsetDb);
    rtaOffsetDb = clamped;
    spectrumOffsetDb = kTraceRtaDefaultOffsetDb + clamped;
    const int shown = juce::roundToInt(clamped);
    rtaOffsetLabel.setText((shown > 0 ? "+" : "") + juce::String(shown), juce::dontSendNotification);
    if (applySpectrumOffset) applySpectrumOffset(clamped);
}

void DeEsserWindow::setWeightingIndex(int index) {
    const int clamped = juce::jlimit(0, 2, index);
    const bool changed = clamped != weightingIndex;
    weightingIndex = clamped;
    weighting = weightingFromIndex(clamped);
    for (int i = 0; i < 3; ++i)
        weightingButtons[static_cast<std::size_t>(i)]->setToggleState(i == clamped);
    if (!changed) return;
    if (applyWeighting) applyWeighting(clamped);
}

void DeEsserWindow::syncControls() {
    freq.setValue(effect.freqHz, juce::dontSendNotification);
    q.setValue(effect.q, juce::dontSendNotification);
    threshold.setValue(effect.thresholdDb, juce::dontSendNotification);
    range.setValue(effect.rangeDb, juce::dontSendNotification);
    attack.setValue(effect.attackMs, juce::dontSendNotification);
    release.setValue(effect.releaseMs, juce::dontSendNotification);
    modeButton.setButtonText(effect.mode == DeEsserMode::Split ? "Split" : "Wideband");
}

void DeEsserWindow::applyPreset(int id) {
    if (id == 2) effect = makeDefaultDeEsserEffect();
    if (id == 3) { effect.freqHz = 6500; effect.q = 2; effect.thresholdDb = -36; effect.rangeDb = 8; }
    if (id == 4) { effect.freqHz = 5500; effect.q = 4; effect.thresholdDb = -24; effect.rangeDb = 18; }
    syncControls(); apply(effect, false);
}

void DeEsserWindow::setBypassed(bool b) { effect.bypassed = b; enableButton.setToggleState(!b, juce::dontSendNotification); }
void DeEsserWindow::setAnalyzerMode(int m) { if (m == 0) stopTimer(); else startTimer(33); }

void DeEsserWindow::timerCallback() {
    if (pullAnalyzerTap) pullAnalyzerTap(tapL.data(), tapR.data(), kAnalyzerFftSize);
    for (int i = 0; i < kAnalyzerFftSize; ++i) {
        const std::size_t idx = static_cast<std::size_t>(i);
        fftScratch[idx] = (tapL[idx] + tapR[idx]) * 0.5f * hannCoeffs[idx];
    }
    analyzerFft->performFrequencyOnlyForwardTransform(fftScratch.data(), true);
    constexpr float normalisation = 4.0f / static_cast<float>(kAnalyzerFftSize);
    for (std::size_t i = 0; i < spectrumDb.size(); ++i) {
        const float magnitude = fftScratch[i] * normalisation;
        spectrumDb[i] = juce::jmax(-100.0f, 20.0f * std::log10(juce::jmax(1.0e-5f, magnitude)));
    }
    for (std::size_t bin = 0; bin < spectrumDb.size(); ++bin) {
        const float fresh = spectrumDb[bin];
        float& held = spectrumAvg[bin];
        held += (fresh - held) * (fresh > held ? 0.6f : 0.15f);
    }
    // Sibilance-band level, not broadband RMS: a 440 Hz voice at -6 dB
    // must not hide a quiet 6 kHz sibilance from the detector (or the bar).
    const float levelDb = deEsserBandLevelDb(
        tapL.data(), tapR.data(), kAnalyzerFftSize, effect, sampleRateHz,
        detS1L, detS2L, detS1R, detS2R);
    gainReductionDb = std::abs(deEsserStaticGainDb(levelDb, effect));
    graph.setFrequencyData(FrequencyTraceData{
        {}, spectrumAvg,
        makeDeEsserGainReductionTrace(effect, gainReductionDb, spectrumAvg.size()),
        sampleRateHz, spectrumOffsetDb, weighting});
    graph.repaint();
}

void DeEsserWindow::resized() {
    auto* installedContent = getContentComponent();
    if (installedContent == nullptr) return;
    installedContent->setBounds(getLocalBounds());
    auto area = installedContent->getLocalBounds().reduced(8);
    auto header = area.removeFromTop(36);
    modeLabel.setBounds(header.removeFromLeft(250).reduced(12, 0));
    presetCombo.setBounds(header.removeFromRight(120));
    header.removeFromRight(6);
    enableButton.setBounds(header.removeFromRight(72));
    area.removeFromTop(6);
    auto analyzerRow = area.removeFromTop(28);
    analyzerLabel.setBounds(analyzerRow.removeFromLeft(60));
    analyzerRow.removeFromLeft(4);
    rtaPlusButton.setBounds(analyzerRow.removeFromRight(28).reduced(1, 1));
    analyzerRow.removeFromRight(2);
    rtaOffsetLabel.setBounds(analyzerRow.removeFromRight(34));
    analyzerRow.removeFromRight(2);
    rtaMinusButton.setBounds(analyzerRow.removeFromRight(28).reduced(1, 1));
    analyzerRow.removeFromRight(6);
    for (int i = 2; i >= 0; --i) {
        weightingButtons[static_cast<std::size_t>(i)]
            ->setBounds(analyzerRow.removeFromRight(24).reduced(1, 1));
        analyzerRow.removeFromRight(2);
    }
    area.removeFromTop(8);
    auto controls = area.removeFromBottom(132);
    area.removeFromBottom(8);
    graph.setBounds(area);
    modeButton.setBounds(controls.removeFromLeft(110));
    controls.removeFromLeft(8);
    const int knobWidth = juce::jmax(70, controls.getWidth() / 6);
    auto placeKnob = [&](ValueEditSlider& slider, juce::Label& label) {
        auto slot = controls.removeFromLeft(knobWidth);
        theme::ui::placeWindowKnob(slot, label, slider);
    };
    placeKnob(freq, freqLabel);
    placeKnob(q, qLabel);
    placeKnob(threshold, thresholdLabel);
    placeKnob(range, rangeLabel);
    placeKnob(attack, attackLabel);
    placeKnob(release, releaseLabel);
}
