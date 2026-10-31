#include "DynamicsWindow.h"

#include <algorithm>
#include <cmath>

DynamicsWindow::DynamicsWindow(
    const juce::String& title,
    const pluginrack::DynamicsEffect& initialEffect,
    std::function<void(const pluginrack::DynamicsEffect&)> applyCallback)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      effect(initialEffect), type(initialEffect.type), apply(std::move(applyCallback)) {
    setUsingNativeTitleBar(true);
    setResizable(false, false);
    content.setSize(420, 220);
    setContentOwned(&content, false);
    modeLabel.setText(type == pluginrack::DynamicsEffectType::Compressor ? "COMPRESSOR" :
                      type == pluginrack::DynamicsEffectType::Limiter ? "LIMITER" :
                      "GATE",
                      juce::dontSendNotification);
    modeLabel.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    modeLabel.setColour(juce::Label::textColourId, theme::cyan());
    content.addAndMakeVisible(modeLabel);
    curve.setEffect(effect);
    curve.setVisible(type == pluginrack::DynamicsEffectType::Compressor);
    content.addAndMakeVisible(curve);
    bypassButton.setFontSize(10);
    bypassButton.setToggleState(!effect.bypassed);
    bypassButton.setClickAction([this] {
        effect.bypassed = !effect.bypassed;
        bypassButton.setToggleState(!effect.bypassed);
        curve.setEffect(effect);
        if (apply) apply(effect);
    });
    content.addAndMakeVisible(bypassButton);

    const auto setup = [this](juce::Slider& slider, double min, double max,
                              double value, const juce::String& suffix,
                              std::function<void(float)> changed) {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 18);
        slider.setRange(min, max, 0.1);
        slider.setValue(value, juce::dontSendNotification);
        slider.setTextValueSuffix(suffix);
        auto* target = &slider;
        slider.onValueChange = [target, changed] {
            changed(static_cast<float>(target->getValue()));
        };
        content.addAndMakeVisible(slider);
    };
    setup(threshold, -60.0, 0.0, effect.thresholdDb, " dB",
          [this](float value) { effect.thresholdDb = value; curve.setEffect(effect); if (apply) apply(effect); });
    setup(ratio, 1.0, 20.0, effect.ratio, ":1",
          [this](float value) { effect.ratio = value; curve.setEffect(effect); if (apply) apply(effect); });
    setup(attack, 0.1, 200.0, effect.attackMs, " ms",
          [this](float value) { effect.attackMs = value; if (apply) apply(effect); });
    setup(release, 1.0, 1000.0, effect.releaseMs, " ms",
          [this](float value) { effect.releaseMs = value; if (apply) apply(effect); });
    setup(makeup, -24.0, 24.0, effect.makeupDb, " dB",
          [this](float value) { effect.makeupDb = value; curve.setEffect(effect); if (apply) apply(effect); });
    setup(inputGain, -24.0, 24.0, effect.inputGainDb, " dB",
          [this](float value) { effect.inputGainDb = value; curve.setEffect(effect); if (apply) apply(effect); });
    setup(knee, 0.0, 24.0, effect.kneeDb, " dB",
          [this](float value) { effect.kneeDb = value; curve.setEffect(effect); if (apply) apply(effect); });
    for (auto* label : {&thresholdLabel, &ratioLabel, &attackLabel, &releaseLabel, &makeupLabel,
                        &inputGainLabel, &kneeLabel}) {
        label->setJustificationType(juce::Justification::centred);
        label->setColour(juce::Label::textColourId, theme::dim());
        content.addAndMakeVisible(*label);
    }
    if (type == pluginrack::DynamicsEffectType::Limiter) {
        thresholdLabel.setText("Ceiling", juce::dontSendNotification);
        ratio.setVisible(false);
        ratioLabel.setVisible(false);
        makeup.setVisible(false);
        makeupLabel.setVisible(false);
        inputGain.setVisible(false);
        inputGainLabel.setVisible(false);
        knee.setVisible(false);
        kneeLabel.setVisible(false);
    } else if (type == pluginrack::DynamicsEffectType::Gate) {
        ratioLabel.setText("Range", juce::dontSendNotification);
        makeup.setVisible(false);
        makeupLabel.setVisible(false);
        inputGain.setVisible(false);
        inputGainLabel.setVisible(false);
        knee.setVisible(false);
        kneeLabel.setVisible(false);
    }
    const bool graphMode = type == pluginrack::DynamicsEffectType::Compressor;
    setContentComponentSize(680, graphMode ? 460 : 250);
    startTimerHz(30);
}

void DynamicsWindow::resized() {
    content.setBounds(getLocalBounds());
    auto area = content.getLocalBounds().reduced(12);
    modeLabel.setBounds(area.removeFromTop(24));
    if (type == pluginrack::DynamicsEffectType::Compressor) {
        curve.setBounds(area.removeFromTop(190));
        area.removeFromTop(8);
    }

    bypassButton.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    auto place = [](juce::Rectangle<int> slot, juce::Label& label, juce::Slider& slider) {
        label.setBounds(slot.removeFromTop(18));
        slider.setBounds(slot.reduced(2));
    };
    auto line = area.removeFromTop(92);
    place(line.removeFromLeft(82), inputGainLabel, inputGain);
    place(line.removeFromLeft(82), thresholdLabel, threshold);
    place(line.removeFromLeft(82), ratioLabel, ratio);
    place(line.removeFromLeft(82), attackLabel, attack);
    place(line.removeFromLeft(82), releaseLabel, release);
    place(line.removeFromLeft(82), kneeLabel, knee);
    place(line.removeFromLeft(82), makeupLabel, makeup);
}

void DynamicsWindow::timerCallback() {
    if (!pullAnalyzerTap) return;
    pullAnalyzerTap(tapL.data(), tapR.data(), static_cast<int>(tapL.size()));

    float inputSum = 0.0f;
    for (std::size_t i = 0; i < tapL.size(); ++i)
        inputSum += std::max(tapL[i] * tapL[i], tapR[i] * tapR[i]);
    const float inputDb = juce::jlimit(-60.0f, 0.0f,
        10.0f * std::log10(std::max(inputSum / static_cast<float>(tapL.size()), 1.0e-6f)));
    float outputDb = inputDb;
    if (inputDb > effect.thresholdDb && effect.ratio > 1.0f)
        outputDb = effect.thresholdDb +
                   (inputDb - effect.thresholdDb) / effect.ratio + effect.makeupDb;
    if (type == pluginrack::DynamicsEffectType::Compressor) {
        curve.setLiveLevels(inputDb, outputDb);
    }
}
