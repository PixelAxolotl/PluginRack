#include "MultibandWindow.h"

#include <algorithm>
#include <cmath>

using namespace pluginrack;

MultibandWindow::MultibandWindow(const juce::String& title,
                                 const MultibandEffect& initialEffect,
                                 ApplyCallback applyCallback,
                                 float initialRtaOffsetDb,
                                 int initialWeighting,
                                 double initialSampleRateHz)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      effect(initialEffect), apply(std::move(applyCallback)),
      sampleRateHz(initialSampleRateHz > 0.0 ? initialSampleRateHz : 48000.0),
      analyzerFft(std::make_unique<juce::dsp::FFT>(13)) {
    MultibandCurveComponent::normaliseEffect(effect);
    setResizable(false, false);
    setContentOwned(&content, false);
    setUsingNativeTitleBar(true);
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

    modeLabel.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    modeLabel.setColour(juce::Label::textColourId, theme::cyan());
    content.addAndMakeVisible(modeLabel);
    content.addAndMakeVisible(analyzerLabel);
    content.addAndMakeVisible(rtaMinusButton);
    content.addAndMakeVisible(rtaOffsetLabel);
    content.addAndMakeVisible(rtaPlusButton);
    content.addAndMakeVisible(weightingZ);
    content.addAndMakeVisible(weightingA);
    content.addAndMakeVisible(weightingC);
    content.addAndMakeVisible(curve);
    content.addAndMakeVisible(enableButton);
    content.addAndMakeVisible(presetCombo);
    content.addAndMakeVisible(bandLabel);
    content.addAndMakeVisible(threshold);
    content.addAndMakeVisible(thresholdLabel);
    content.addAndMakeVisible(ratio);
    content.addAndMakeVisible(ratioLabel);
    content.addAndMakeVisible(attack);
    content.addAndMakeVisible(attackLabel);
    content.addAndMakeVisible(release);
    content.addAndMakeVisible(releaseLabel);
    content.addAndMakeVisible(knee);
    content.addAndMakeVisible(kneeLabel);
    content.addAndMakeVisible(makeup);
    content.addAndMakeVisible(makeupLabel);
    content.addAndMakeVisible(inputGain);
    content.addAndMakeVisible(inputGainLabel);
    content.addAndMakeVisible(modeButton);
    content.addAndMakeVisible(soloButton);
    content.addAndMakeVisible(muteButton);
    content.addAndMakeVisible(addBandButton);
    content.addAndMakeVisible(removeBandButton);
    content.addAndMakeVisible(autoMakeupButton);

    curve.setEffect(effect);
    curve.onBandSelected = [this] { selectBand(curve.getSelectedBand()); };
    curve.onEffectChanged = [this](const MultibandEffect& changed) {
        effect = changed;
        syncControls();
        if (apply) apply(effect, false);
    };

    enableButton.setClickingTogglesState(true);
    enableButton.setToggleState(!effect.bypassed, juce::dontSendNotification);
    theme::ui::styleButton(enableButton);
    theme::ui::styleButton(modeButton);
    theme::ui::styleButton(soloButton);
    theme::ui::styleButton(muteButton);
    theme::ui::styleButton(addBandButton);
    theme::ui::styleButton(removeBandButton);
    theme::ui::styleButton(autoMakeupButton);
    theme::ui::styleCombo(presetCombo);
    enableButton.setClickingTogglesState(true);
    enableButton.onClick = [this] {
        effect.bypassed = !enableButton.getToggleState();
        curve.setEffect(effect);
        if (apply) apply(effect, true);
    };

    presetCombo.addItem("Custom", 1);
    presetCombo.addItem("Default", 2);
    presetCombo.addItem("Gentle", 3);
    presetCombo.addItem("Tight", 4);
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
    curve.setSpectrumWeighting(SpectrumWeighting::Z);
    setWeightingIndex(initialWeighting);
    setRtaOffsetDb(initialRtaOffsetDb);

    auto styleRotary = [](ValueEditSlider& slider, double min, double max,
                          const juce::String& suffix, juce::Colour fill) {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 20);
        slider.setRange(min, max, 0.1);
        slider.setTextValueSuffix(suffix);
        slider.setDoubleClickReturnValue(true, min <= 0.0 && max >= 0.0 ? 0.0 : min);
        slider.setColour(juce::Slider::rotarySliderFillColourId, fill);
        slider.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
        slider.setColour(juce::Slider::textBoxTextColourId, theme::text());
        slider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
        slider.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    };
    styleRotary(inputGain, -24.0, 24.0, " dB", theme::cyan());
    styleRotary(threshold, -60.0, 0.0, " dB", theme::cyan());
    styleRotary(ratio, 1.0, 20.0, ":1", theme::green());
    styleRotary(attack, 0.1, 200.0, " ms", theme::amber());
    styleRotary(release, 1.0, 1000.0, " ms", theme::violet());
    styleRotary(knee, 0.0, 24.0, " dB", theme::blue());
    styleRotary(makeup, -24.0, 24.0, " dB", theme::green());

    threshold.onValueChange = [this] {
        if (selectedBand >= 0 && selectedBand < static_cast<int>(effect.bands.size())) {
            effect.bands[static_cast<std::size_t>(selectedBand)].thresholdDb =
                static_cast<float>(threshold.getValue());
            curve.setEffect(effect);
            if (apply) apply(effect, false);
        }
    };
    ratio.onValueChange = [this] {
        if (selectedBand >= 0 && selectedBand < static_cast<int>(effect.bands.size())) {
            effect.bands[static_cast<std::size_t>(selectedBand)].ratio =
                static_cast<float>(ratio.getValue());
            curve.setEffect(effect);
            if (apply) apply(effect, false);
        }
    };
    attack.onValueChange = [this] {
        if (selectedBand >= 0 && selectedBand < static_cast<int>(effect.bands.size())) {
            effect.bands[static_cast<std::size_t>(selectedBand)].attackMs =
                static_cast<float>(attack.getValue());
            curve.setEffect(effect);
            if (apply) apply(effect, false);
        }
    };
    release.onValueChange = [this] {
        if (selectedBand >= 0 && selectedBand < static_cast<int>(effect.bands.size())) {
            effect.bands[static_cast<std::size_t>(selectedBand)].releaseMs =
                static_cast<float>(release.getValue());
            curve.setEffect(effect);
            if (apply) apply(effect, false);
        }
    };
    knee.onValueChange = [this] {
        if (selectedBand >= 0 && selectedBand < static_cast<int>(effect.bands.size())) {
            effect.bands[static_cast<std::size_t>(selectedBand)].kneeDb =
                static_cast<float>(knee.getValue());
            curve.setEffect(effect);
            if (apply) apply(effect, false);
        }
    };
    makeup.onValueChange = [this] {
        if (selectedBand >= 0 && selectedBand < static_cast<int>(effect.bands.size())) {
            effect.bands[static_cast<std::size_t>(selectedBand)].makeupDb =
                static_cast<float>(makeup.getValue());
            curve.setEffect(effect);
            if (apply) apply(effect, false);
        }
    };
    inputGain.onValueChange = [this] {
        if (selectedBand >= 0 && selectedBand < static_cast<int>(effect.bands.size())) {
            effect.bands[static_cast<std::size_t>(selectedBand)].inputGainDb =
                static_cast<float>(inputGain.getValue());
            curve.setEffect(effect);
            if (apply) apply(effect, false);
        }
    };

    for (auto* label : {&thresholdLabel, &ratioLabel, &attackLabel, &releaseLabel,
                        &kneeLabel, &makeupLabel, &inputGainLabel}) {
        label->setJustificationType(juce::Justification::centred);
        label->setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        label->setColour(juce::Label::textColourId, theme::dim());
    }

    modeButton.onClick = [this] {
        if (selectedBand < 0 || selectedBand >= static_cast<int>(effect.bands.size())) return;
        auto& band = effect.bands[static_cast<std::size_t>(selectedBand)];
        band.mode = band.mode == MultibandMode::Compress ? MultibandMode::Expand
                                                          : MultibandMode::Compress;
        syncControls();
        curve.setEffect(effect);
        if (apply) apply(effect, false);
    };
    soloButton.setClickingTogglesState(true);
    soloButton.onClick = [this] {
        if (selectedBand < 0 || selectedBand >= static_cast<int>(effect.bands.size())) return;
        effect.bands[static_cast<std::size_t>(selectedBand)].soloed = soloButton.getToggleState();
        curve.setEffect(effect);
        if (apply) apply(effect, false);
    };
    muteButton.setClickingTogglesState(true);
    muteButton.onClick = [this] {
        if (selectedBand < 0 || selectedBand >= static_cast<int>(effect.bands.size())) return;
        effect.bands[static_cast<std::size_t>(selectedBand)].bypassed = muteButton.getToggleState();
        curve.setEffect(effect);
        if (apply) apply(effect, false);
    };
    addBandButton.onClick = [this] {
        if (effect.bands.size() >= 6) return;
        effect.bands.push_back(MultibandBand{});
        const float previous = effect.crossovers.empty() ? 200.0f : effect.crossovers.back();
        effect.crossovers.push_back(std::min(18000.0f, previous * 2.0f));
        MultibandCurveComponent::normaliseEffect(effect);
        selectBand(static_cast<int>(effect.bands.size()) - 1);
        curve.setEffect(effect);
        if (apply) apply(effect, false);
    };
    removeBandButton.onClick = [this] {
        if (effect.bands.size() <= 2) return;
        effect.bands.pop_back();
        effect.crossovers.pop_back();
        MultibandCurveComponent::normaliseEffect(effect);
        selectBand(std::min(selectedBand, static_cast<int>(effect.bands.size()) - 1));
        curve.setEffect(effect);
        if (apply) apply(effect, false);
    };
    autoMakeupButton.setClickingTogglesState(true);
    autoMakeupButton.setToggleState(effect.autoMakeup, juce::dontSendNotification);
    autoMakeupButton.onClick = [this] {
        effect.autoMakeup = autoMakeupButton.getToggleState();
        curve.setEffect(effect);
        if (apply) apply(effect, false);
    };

    syncControls();
    setVisible(true);
    startTimer(33);
}

void MultibandWindow::setRtaOffsetDb(float offsetDb) {
    const float clamped = juce::jlimit(kTraceRtaMinOffsetDb, kTraceRtaMaxOffsetDb, offsetDb);
    rtaOffsetDb = clamped;
    spectrumOffsetDb = kTraceRtaDefaultOffsetDb + clamped;
    curve.setSpectrumOffsetDb(spectrumOffsetDb);
    const int shown = juce::roundToInt(clamped);
    rtaOffsetLabel.setText((shown > 0 ? "+" : "") + juce::String(shown), juce::dontSendNotification);
    if (applySpectrumOffset) applySpectrumOffset(clamped);
}

void MultibandWindow::setWeightingIndex(int index) {
    const int clamped = juce::jlimit(0, 2, index);
    const bool changed = clamped != weightingIndex;
    weightingIndex = clamped;
    weighting = weightingFromIndex(clamped);
    for (int i = 0; i < 3; ++i)
        weightingButtons[static_cast<std::size_t>(i)]->setToggleState(i == clamped);
    if (!changed) return;
    curve.setSpectrumWeighting(weighting);
    if (applyWeighting) applyWeighting(clamped);
}

void MultibandWindow::selectBand(int index) {
    if (index < 0 || index >= static_cast<int>(effect.bands.size())) return;
    selectedBand = index;
    curve.setSelectedBand(index);
    syncControls();
}

void MultibandWindow::syncControls() {
    if (selectedBand < 0 || selectedBand >= static_cast<int>(effect.bands.size())) return;
    const auto& band = effect.bands[static_cast<std::size_t>(selectedBand)];
    threshold.setValue(band.thresholdDb, juce::dontSendNotification);
    ratio.setValue(band.ratio, juce::dontSendNotification);
    attack.setValue(band.attackMs, juce::dontSendNotification);
    release.setValue(band.releaseMs, juce::dontSendNotification);
    knee.setValue(band.kneeDb, juce::dontSendNotification);
    makeup.setValue(band.makeupDb, juce::dontSendNotification);
    inputGain.setValue(band.inputGainDb, juce::dontSendNotification);
    modeButton.setButtonText(band.mode == MultibandMode::Compress ? "Compress" : "Expand");
    soloButton.setToggleState(band.soloed, juce::dontSendNotification);
    muteButton.setToggleState(band.bypassed, juce::dontSendNotification);
    bandLabel.setText("Band " + juce::String(selectedBand + 1) + " / "
                          + juce::String(static_cast<int>(effect.bands.size())),
                      juce::dontSendNotification);
}

void MultibandWindow::applyPreset(int id) {
    if (id == 2) {
        effect = makeDefaultMultibandEffect(effect.id);
    } else if (id == 3) {
        for (auto& band : effect.bands) {
            band.thresholdDb = -18.0f;
            band.ratio = 2.0f;
            band.attackMs = 20.0f;
            band.releaseMs = 200.0f;
        }
    } else if (id == 4) {
        for (auto& band : effect.bands) {
            band.thresholdDb = -12.0f;
            band.ratio = 8.0f;
            band.attackMs = 3.0f;
            band.releaseMs = 80.0f;
        }
    }
    MultibandCurveComponent::normaliseEffect(effect);
    curve.setEffect(effect);
    syncControls();
    if (apply) apply(effect, false);
}

void MultibandWindow::setBypassed(bool bypassed) {
    effect.bypassed = bypassed;
    enableButton.setToggleState(!bypassed, juce::dontSendNotification);
    curve.setEffect(effect);
}

void MultibandWindow::setAnalyzerMode(int mode) {
    if (mode == 0) {
        stopTimer();
        curve.setSpectrum(nullptr, 0);
    } else {
        startTimer(33);
    }
}

void MultibandWindow::timerCallback() {
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
    curve.setSpectrum(spectrumAvg.data(), static_cast<int>(spectrumAvg.size()), sampleRateHz);

    float sum = 0.0f;
    for (int i = 0; i < kAnalyzerFftSize; ++i) {
        const float sample = (tapL[static_cast<std::size_t>(i)] + tapR[static_cast<std::size_t>(i)]) * 0.5f;
        sum += sample * sample;
    }
    const float levelDb = 20.0f * std::log10(std::max(1.0e-5f, std::sqrt(sum / kAnalyzerFftSize)));
    gainReductionDb.resize(effect.bands.size());
    for (std::size_t i = 0; i < effect.bands.size(); ++i) {
        const float gain = MultibandCurveComponent::compressionGainDb(levelDb, effect.bands[i]);
        gainReductionDb[i] = std::max(0.0f, -gain);
    }
    curve.setGainReduction(gainReductionDb);
}

void MultibandWindow::resized() {
    auto* installedContent = getContentComponent();
    if (installedContent == nullptr) return;
    installedContent->setBounds(getLocalBounds());
    auto area = installedContent->getLocalBounds().reduced(8);

    auto header = area.removeFromTop(28);
    modeLabel.setBounds(header.removeFromLeft(250));
    autoMakeupButton.setBounds(header.removeFromRight(112));
    header.removeFromRight(6);
    presetCombo.setBounds(header.removeFromRight(120));
    header.removeFromRight(6);
    enableButton.setBounds(header.removeFromRight(72));

    auto controls = area.removeFromBottom(132);
    area.removeFromBottom(8);
    auto analyzerRow = area.removeFromTop(24);
    analyzerRow.removeFromBottom(3);
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
    area.removeFromTop(6);
    curve.setBounds(area);

    auto command = controls.removeFromLeft(204);
    bandLabel.setBounds(command.removeFromTop(20));
    command.removeFromTop(4);
    modeButton.setBounds(command.removeFromTop(24));
    command.removeFromTop(4);
    auto buttonRow = command.removeFromTop(24);
    soloButton.setBounds(buttonRow.removeFromLeft(58));
    buttonRow.removeFromLeft(4);
    muteButton.setBounds(buttonRow.removeFromLeft(58));
    buttonRow.removeFromLeft(4);
    addBandButton.setBounds(buttonRow.removeFromLeft(62));
    command.removeFromTop(4);
    auto removeRow = command.removeFromTop(24);
    removeBandButton.setBounds(removeRow.removeFromLeft(80));

    controls.removeFromLeft(8);
    const int knobWidth = juce::jmax(70, controls.getWidth() / 7);
    auto placeKnob = [&](ValueEditSlider& slider, juce::Label& label) {
        auto slot = controls.removeFromLeft(knobWidth);
        label.setBounds(slot.removeFromTop(16));
        slider.setBounds(slot.reduced(2, 0));
    };
    placeKnob(inputGain, inputGainLabel);
    placeKnob(threshold, thresholdLabel);
    placeKnob(ratio, ratioLabel);
    placeKnob(attack, attackLabel);
    placeKnob(release, releaseLabel);
    placeKnob(knee, kneeLabel);
    placeKnob(makeup, makeupLabel);
}
