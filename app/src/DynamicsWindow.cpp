#include "DynamicsWindow.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
struct Preset {
    const char* name;
    float thresholdDb;
    float ratio;
    float attackMs;
    float releaseMs;
    float kneeDb;
    float makeupDb;
    float inputGainDb;
};

// "Default" is the factory setting for the effect type, so it stays in step
// with defaultDynamicsEffects() instead of being a second copy of the numbers.
Preset defaultPreset(pluginrack::DynamicsEffectType type) {
    for (const auto& effect : pluginrack::defaultDynamicsEffects())
        if (effect.type == type)
            return Preset{ "Default", effect.thresholdDb, effect.ratio, effect.attackMs,
                           effect.releaseMs, effect.kneeDb, effect.makeupDb, effect.inputGainDb };
    return Preset{ "Default", 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
}

bool presetMatches(const Preset& preset, const pluginrack::DynamicsEffect& effect) {
    const auto close = [](float a, float b) { return std::abs(a - b) < 0.05f; };
    return close(preset.thresholdDb, effect.thresholdDb) &&
           close(preset.ratio, effect.ratio) &&
           close(preset.attackMs, effect.attackMs) &&
           close(preset.releaseMs, effect.releaseMs) &&
           close(preset.kneeDb, effect.kneeDb) &&
           close(preset.makeupDb, effect.makeupDb) &&
           close(preset.inputGainDb, effect.inputGainDb);
}

std::vector<Preset> presetsFor(pluginrack::DynamicsEffectType type) {
    static const std::vector<Preset> compressor{
        { "Vocal",    -20.0f,  3.0f,  8.0f, 120.0f, 6.0f, 4.0f, 0.0f },
        { "Drum Bus", -14.0f,  4.0f, 25.0f, 150.0f, 3.0f, 5.0f, 0.0f },
        { "Mix Bus",  -22.0f,  2.0f, 40.0f, 300.0f, 8.0f, 3.0f, 0.0f },
        { "Squash",   -30.0f, 12.0f,  3.0f,  80.0f, 0.0f, 8.0f, 0.0f },
    };
    static const std::vector<Preset> gate{
        { "Tight",  -30.0f, 60.0f, 0.5f,  60.0f, 0.0f, 0.0f, 0.0f },
        { "Gentle", -45.0f, 20.0f, 5.0f, 250.0f, 0.0f, 0.0f, 0.0f },
    };
    const std::vector<Preset>* extras = &compressor;
    switch (type) {
        case pluginrack::DynamicsEffectType::Compressor: break;
        case pluginrack::DynamicsEffectType::Gate: extras = &gate; break;
    }
    std::vector<Preset> presets{ defaultPreset(type) };
    presets.insert(presets.end(), extras->begin(), extras->end());
    return presets;
}

constexpr int kCustomPresetId = 1;
// The dot's simulated attack/release run on this same tick.
constexpr float kTimerHz = 60.0f;
}

DynamicsWindow::DynamicsWindow(
    const juce::String& title,
    const pluginrack::DynamicsEffect& initialEffect,
    ApplyCallback applyCallback,
    const pluginrack::DynamicsEffect* gate)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      effect(initialEffect), type(initialEffect.type), apply(std::move(applyCallback)) {
    setUsingNativeTitleBar(true);
    setResizable(false, false);
    setContentOwned(&content, false);
    // Used as-is when this opens as a floating window (the mixer strips): open
    // in the middle of the screen instead of JUCE's default top-left corner.
    // The rack docks it into the editor bay and overrides the bounds.
    centreWithSize(kMinWidth, kMinHeight);
    // The gate row edits the strip's gate effect, which lives alongside the
    // compressor; only the compressor panel shows it.
    hasGate = type == pluginrack::DynamicsEffectType::Compressor && gate != nullptr;
    if (hasGate) gateEffect = *gate;
    modeLabel.setText("DYNAMICS", juce::dontSendNotification);
    theme::ui::styleTitleLabel(modeLabel);
    content.addAndMakeVisible(modeLabel);
    curve.setEffect(effect);
    curve.setVisible(type == pluginrack::DynamicsEffectType::Compressor);
    content.addAndMakeVisible(curve);
    gauge.setVisible(true);
    content.addAndMakeVisible(gauge);

    presetCombo.addItem("Custom", kCustomPresetId);
    const auto presets = presetsFor(type);
    int presetId = kCustomPresetId + 1;
    int selectedPresetId = kCustomPresetId;
    for (const auto& preset : presets) {
        presetCombo.addItem(preset.name, presetId);
        if (presetMatches(preset, effect)) selectedPresetId = presetId;
        ++presetId;
    }
    presetCombo.setSelectedId(selectedPresetId, juce::dontSendNotification);
    presetCombo.onChange = [this] { applyPreset(presetCombo.getSelectedId()); };
    presetCombo.setColour(juce::ComboBox::backgroundColourId, theme::control());
    presetCombo.setColour(juce::ComboBox::textColourId, theme::text());
    presetCombo.setColour(juce::ComboBox::arrowColourId, theme::dim());
    presetCombo.setColour(juce::ComboBox::outlineColourId, theme::border());
    content.addAndMakeVisible(presetCombo);

    theme::ui::styleButton(enableButton);
    enableButton.setClickingTogglesState(true);
    enableButton.setToggleState(!effect.bypassed, juce::dontSendNotification);
    enableButton.setTooltip(type == pluginrack::DynamicsEffectType::Compressor
                                ? "Enable or bypass the compressor"
                                : "Enable or bypass the gate");
    enableButton.onClick = [this] {
        effect.bypassed = !enableButton.getToggleState();
        curve.setEffect(effect);
        // true: the switch itself moved, so the host must not restore the
        // bypass state it had before this edit.
        if (apply) apply(effect, true);
    };
    content.addAndMakeVisible(enableButton);

    // The gate has its own preset list so the compressor's choice never
    // rewrites gate settings (and vice versa).
    if (hasGate) {
        const auto gatePresets = presetsFor(pluginrack::DynamicsEffectType::Gate);
        gatePresetCombo.addItem("Custom", kCustomPresetId);
        int gatePresetId = kCustomPresetId + 1;
        int gateSelectedId = kCustomPresetId;
        for (const auto& preset : gatePresets) {
            gatePresetCombo.addItem(preset.name, gatePresetId);
            if (presetMatches(preset, gateEffect)) gateSelectedId = gatePresetId;
            ++gatePresetId;
        }
        gatePresetCombo.setSelectedId(gateSelectedId, juce::dontSendNotification);
        gatePresetCombo.onChange = [this] { applyGatePreset(gatePresetCombo.getSelectedId()); };
        gatePresetCombo.setColour(juce::ComboBox::backgroundColourId, theme::control());
        gatePresetCombo.setColour(juce::ComboBox::textColourId, theme::text());
        gatePresetCombo.setColour(juce::ComboBox::arrowColourId, theme::dim());
        gatePresetCombo.setColour(juce::ComboBox::outlineColourId, theme::border());
        content.addAndMakeVisible(gatePresetCombo);
    }

    const auto markCustom = [this] {
        presetCombo.setSelectedId(kCustomPresetId, juce::dontSendNotification);
    };
    const auto markGateCustom = [this] {
        gatePresetCombo.setSelectedId(kCustomPresetId, juce::dontSendNotification);
    };
    const auto styleSlider = [this](juce::Slider& slider, double min, double max, double value,
                                    const juce::String& suffix, double resetValue, juce::Colour fill) {
        theme::ui::styleWindowKnob(slider, min, max, 0.1, suffix, resetValue, fill);
        slider.setValue(value, juce::dontSendNotification);
        content.addAndMakeVisible(slider);
    };
    const auto setup = [this, markCustom, styleSlider](juce::Slider& slider, double min,
                                                       double max, double value,
                                                       const juce::String& suffix,
                                                       double resetValue, juce::Colour fill,
                                                       std::function<void(float)> changed) {
        styleSlider(slider, min, max, value, suffix, resetValue, fill);
        auto* target = &slider;
        slider.onValueChange = [target, changed, markCustom] {
            markCustom();
            changed(static_cast<float>(target->getValue()));
        };
    };
    // Gate knobs must not rename the compressor's preset.
    const auto setupGate = [this, markGateCustom, styleSlider](juce::Slider& slider, double min,
                                                               double max, double value,
                                                               const juce::String& suffix,
                                                               double resetValue, juce::Colour fill,
                                                               std::function<void(float)> changed) {
        styleSlider(slider, min, max, value, suffix, resetValue, fill);
        auto* target = &slider;
        slider.onValueChange = [target, changed, markGateCustom] {
            markGateCustom();
            changed(static_cast<float>(target->getValue()));
        };
    };
    // Double-click resets a knob to this effect type's session default.
    pluginrack::DynamicsEffect defaults;
    for (const auto& candidate : pluginrack::defaultDynamicsEffects())
        if (candidate.type == type) defaults = candidate;
    setup(threshold, -60.0, 0.0, effect.thresholdDb, " dB", defaults.thresholdDb, theme::cyan(),
          [this](float value) { effect.thresholdDb = value; curve.setEffect(effect); if (apply) apply(effect, false); });
    setup(ratio, 1.0, 20.0, effect.ratio,
          type == pluginrack::DynamicsEffectType::Gate ? " dB" : ":1", defaults.ratio, theme::green(),
          [this](float value) { effect.ratio = value; curve.setEffect(effect); if (apply) apply(effect, false); });
    setup(attack, 0.1, 200.0, effect.attackMs, " ms", defaults.attackMs, theme::amber(),
          [this](float value) { effect.attackMs = value; if (apply) apply(effect, false); });
    setup(release, 10.0, 1000.0, effect.releaseMs, " ms", defaults.releaseMs, theme::violet(),
          [this](float value) { effect.releaseMs = value; if (apply) apply(effect, false); });
    setup(makeup, -24.0, 24.0, effect.makeupDb, " dB", defaults.makeupDb, theme::green(),
          [this](float value) { effect.makeupDb = value; curve.setEffect(effect); if (apply) apply(effect, false); });
    setup(inputGain, -24.0, 24.0, effect.inputGainDb, " dB", defaults.inputGainDb, theme::cyan(),
          [this](float value) { effect.inputGainDb = value; curve.setEffect(effect); if (apply) apply(effect, false); });
    setup(knee, 0.0, 24.0, effect.kneeDb, " dB", defaults.kneeDb, theme::violet(),
          [this](float value) { effect.kneeDb = value; curve.setEffect(effect); if (apply) apply(effect, false); });
    for (auto* label : {&thresholdLabel, &ratioLabel, &attackLabel, &releaseLabel, &makeupLabel,
                        &inputGainLabel, &kneeLabel}) {
        label->setJustificationType(juce::Justification::centred);
        label->setColour(juce::Label::textColourId, theme::dim());
        content.addAndMakeVisible(*label);
    }
    // Names for the rack's touch-and-slide strip ("linked to ...").
    threshold.setName("Threshold");
    ratio.setName(type == pluginrack::DynamicsEffectType::Gate ? "Range" : "Ratio");
    attack.setName("Attack");
    release.setName("Release");
    makeup.setName("Output");
    inputGain.setName("Input");
    knee.setName("Knee");

    // Second row: the same strip's gate, so both panels drive one effect.
    if (hasGate) {
        pluginrack::DynamicsEffect gateDefaults;
        for (const auto& candidate : pluginrack::defaultDynamicsEffects())
            if (candidate.type == pluginrack::DynamicsEffectType::Gate)
                gateDefaults = candidate;
        gateCaption.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        gateCaption.setColour(juce::Label::textColourId, theme::dim());
        content.addAndMakeVisible(gateCaption);

        theme::ui::styleButton(gateEnableButton);
        gateEnableButton.setClickingTogglesState(true);
        gateEnableButton.setToggleState(!gateEffect.bypassed, juce::dontSendNotification);
        gateEnableButton.setTooltip("Enable or bypass the gate");
        gateEnableButton.onClick = [this] {
            gateEffect.bypassed = !gateEnableButton.getToggleState();
            curve.setGate(gateEffect, !gateEffect.bypassed);
            // true: the switch itself moved, so the host must not restore the
            // bypass state it had before this edit.
            if (apply) apply(gateEffect, true);
        };
        content.addAndMakeVisible(gateEnableButton);

        for (auto* label : {&gateThresholdLabel, &gateRangeLabel, &gateAttackLabel,
                            &gateReleaseLabel}) {
            label->setJustificationType(juce::Justification::centred);
            label->setColour(juce::Label::textColourId, theme::dim());
            content.addAndMakeVisible(*label);
        }
        const auto gateChanged = [this] {
            curve.setGate(gateEffect, !gateEffect.bypassed);
            if (apply) apply(gateEffect, false);
        };
        setupGate(gateThreshold, -60.0, 0.0, gateEffect.thresholdDb, " dB",
                  gateDefaults.thresholdDb, theme::cyan(),
                  [this, gateChanged](float value) { gateEffect.thresholdDb = value; gateChanged(); });
        setupGate(gateRange, 1.0, 80.0, gateEffect.ratio, " dB", gateDefaults.ratio, theme::green(),
                  [this, gateChanged](float value) { gateEffect.ratio = value; gateChanged(); });
        setupGate(gateAttack, 0.1, 200.0, gateEffect.attackMs, " ms", gateDefaults.attackMs, theme::amber(),
                  [this, gateChanged](float value) { gateEffect.attackMs = value; gateChanged(); });
        setupGate(gateRelease, 10.0, 1000.0, gateEffect.releaseMs, " ms", gateDefaults.releaseMs, theme::violet(),
                  [this, gateChanged](float value) { gateEffect.releaseMs = value; gateChanged(); });
        curve.setGate(gateEffect, !gateEffect.bypassed);
    }
    gateThreshold.setName("Gate Thresh");
    gateRange.setName("Gate Range");
    gateAttack.setName("Gate Attack");
    gateRelease.setName("Gate Rel");
    if (type == pluginrack::DynamicsEffectType::Gate) {
        ratioLabel.setText("Range", juce::dontSendNotification);
        makeup.setVisible(false);
        makeupLabel.setVisible(false);
        inputGain.setVisible(false);
        inputGainLabel.setVisible(false);
        knee.setVisible(false);
        kneeLabel.setVisible(false);
    }
    // Match the other four built-in windows (860x560) so the header, knob row,
    // and curve are the same size everywhere. The old 780x520 and 680x250 sizes
    // were smaller than the row math below, so the 132 px knob rows ran past
    // the bottom edge and only the tops of the knobs showed. The gate row is
    // laid out inside the same 560 px height: a 132 px compressor row on top
    // of a 132 px gate row, plus padding, with the curve between them. That
    // keeps every built-in window the same height, so switching between them
    // never changes the panel size or leaves a black bar behind.
    setContentComponentSize(860, 560);
    startTimerHz(static_cast<int>(kTimerHz));
}

void DynamicsWindow::applyPreset(int itemId) {
    const auto presets = presetsFor(type);
    const int index = itemId - kCustomPresetId - 1;
    if (index < 0 || index >= static_cast<int>(presets.size())) return;
    const auto& preset = presets[static_cast<std::size_t>(index)];
    effect.thresholdDb = preset.thresholdDb;
    effect.ratio = preset.ratio;
    effect.attackMs = preset.attackMs;
    effect.releaseMs = preset.releaseMs;
    effect.kneeDb = preset.kneeDb;
    effect.makeupDb = preset.makeupDb;
    effect.inputGainDb = preset.inputGainDb;
    syncControls();
    curve.setEffect(effect);
    if (apply) apply(effect, false);
}

void DynamicsWindow::applyGatePreset(int itemId) {
    if (!hasGate) return;
    const auto presets = presetsFor(pluginrack::DynamicsEffectType::Gate);
    const int index = itemId - kCustomPresetId - 1;
    if (index < 0 || index >= static_cast<int>(presets.size())) return;
    const auto& preset = presets[static_cast<std::size_t>(index)];
    gateEffect.thresholdDb = preset.thresholdDb;
    gateEffect.ratio = preset.ratio;
    gateEffect.attackMs = preset.attackMs;
    gateEffect.releaseMs = preset.releaseMs;
    gateEffect.kneeDb = preset.kneeDb;
    syncGateControls();
    curve.setGate(gateEffect, !gateEffect.bypassed);
    if (apply) apply(gateEffect, false);
}

void DynamicsWindow::syncControls() {
    const std::array<std::pair<juce::Slider*, float>, 7> values{{
        { &inputGain, effect.inputGainDb },
        { &threshold, effect.thresholdDb },
        { &ratio, effect.ratio },
        { &attack, effect.attackMs },
        { &release, effect.releaseMs },
        { &knee, effect.kneeDb },
        { &makeup, effect.makeupDb },
    }};
    for (const auto& [slider, value] : values)
        slider->setValue(static_cast<double>(value), juce::dontSendNotification);
    enableButton.setToggleState(!effect.bypassed, juce::dontSendNotification);
}

void DynamicsWindow::resized() {
    content.setBounds(getLocalBounds());
    auto area = content.getLocalBounds().reduced(8);

    auto header = area.removeFromTop(36);
    modeLabel.setBounds(header.removeFromLeft(250).reduced(12, 0));
    presetCombo.setBounds(header.removeFromRight(120));
    header.removeFromRight(6);
    enableButton.setBounds(header.removeFromRight(72));
    area.removeFromTop(6);

    const std::vector<std::pair<juce::Label*, juce::Slider*>> mainKnobs{
        { &inputGainLabel, &inputGain },
        { &thresholdLabel, &threshold },
        { &ratioLabel, &ratio },
        { &attackLabel, &attack },
        { &releaseLabel, &release },
        { &kneeLabel, &knee },
        { &makeupLabel, &makeup },
    };
    const std::vector<std::pair<juce::Label*, juce::Slider*>> gateKnobs{
        { &gateThresholdLabel, &gateThreshold },
        { &gateRangeLabel, &gateRange },
        { &gateAttackLabel, &gateAttack },
        { &gateReleaseLabel, &gateRelease },
    };

    if (type == pluginrack::DynamicsEffectType::Compressor) {
        if (hasGate) {
            // The gate row needs its own breathing room: a labelled header
            // separated from both the compressor knobs above and its own
            // knobs below, instead of a caption wedged against the combo.
            auto gateRow = area.removeFromBottom(132);
            {
                const int gateSlotW = juce::jmax(1, gateRow.getWidth() / 4);
                for (int i = 0; i < 4; ++i) {
                    auto slot = juce::Rectangle<int>(gateRow.getX() + i * gateSlotW,
                                                     gateRow.getY(), gateSlotW,
                                                     gateRow.getHeight());
                    theme::ui::placeWindowKnob(slot, *gateKnobs[(std::size_t)i].first,
                                               *gateKnobs[(std::size_t)i].second);
                }
            }
            area.removeFromBottom(8);
            auto gateHeader = area.removeFromBottom(26);
            gateHeader.removeFromLeft(2);
            gateCaption.setBounds(gateHeader.removeFromLeft(58));
            gateHeader.removeFromLeft(6);
            gatePresetCombo.setBounds(gateHeader.removeFromLeft(176));
            gateHeader.removeFromLeft(10);
            gateEnableButton.setBounds(gateHeader.removeFromLeft(72).withSizeKeepingCentre(72, 22));
            area.removeFromBottom(8);
        }
        auto knobsRow = area.removeFromBottom(132);
        area.removeFromBottom(8);

        gauge.setBounds(area.removeFromRight(juce::jmin(260, area.getWidth() / 2)));
        area.removeFromRight(8);
        curve.setBounds(area);
        {
            const int slotW = juce::jmax(1, knobsRow.getWidth() / 7);
            for (int i = 0; i < 7; ++i) {
                auto slot = juce::Rectangle<int>(knobsRow.getX() + i * slotW,
                                                 knobsRow.getY(), slotW,
                                                 knobsRow.getHeight());
                theme::ui::placeWindowKnob(slot, *mainKnobs[(std::size_t)i].first,
                                           *mainKnobs[(std::size_t)i].second);
            }
        }
    } else {
        auto knobsRow = area.removeFromTop(132);
        area.removeFromTop(8);
        gauge.setBounds(area.removeFromRight(180));
        area.removeFromRight(8);
        {
            const int slotW = juce::jmax(1, knobsRow.getWidth() / 7);
            for (int i = 0; i < 7; ++i) {
                auto slot = juce::Rectangle<int>(knobsRow.getX() + i * slotW,
                                                 knobsRow.getY(), slotW,
                                                 knobsRow.getHeight());
                theme::ui::placeWindowKnob(slot, *mainKnobs[(std::size_t)i].first,
                                           *mainKnobs[(std::size_t)i].second);
            }
        }
        curve.setBounds(area);
    }
}

void DynamicsWindow::setBypassed(bool bypassed) {
    effect.bypassed = bypassed;
    enableButton.setToggleState(!effect.bypassed, juce::dontSendNotification);
    curve.setEffect(effect);
}

void DynamicsWindow::setGate(const pluginrack::DynamicsEffect& newGate) {
    if (!hasGate) return;
    gateEffect = newGate;
    syncGateControls();
    curve.setGate(gateEffect, !gateEffect.bypassed);
}

void DynamicsWindow::syncGateControls() {
    gateThreshold.setValue(static_cast<double>(gateEffect.thresholdDb), juce::dontSendNotification);
    gateRange.setValue(static_cast<double>(gateEffect.ratio), juce::dontSendNotification);
    gateAttack.setValue(static_cast<double>(gateEffect.attackMs), juce::dontSendNotification);
    gateRelease.setValue(static_cast<double>(gateEffect.releaseMs), juce::dontSendNotification);
    gateEnableButton.setToggleState(!gateEffect.bypassed, juce::dontSendNotification);
}

void DynamicsWindow::timerCallback() {
    if (!pullAnalyzerTap) return;
    pullAnalyzerTap(tapL.data(), tapR.data(), static_cast<int>(tapL.size()));

    float peak = 0.0f;
    for (std::size_t i = 0; i < tapL.size(); ++i)
        peak = std::max(peak, std::max(std::abs(tapL[i]), std::abs(tapR[i])));
    const float inputDb = juce::jlimit(-60.0f, 0.0f,
        20.0f * std::log10(std::max(peak, 1.0e-6f)));

    // The tap sits before the effect, so the dot starts at the trimmed input.
    const float trimDb = effect.bypassed ? 0.0f : effect.inputGainDb;
    const float dotInputDb = juce::jlimit(-60.0f, 0.0f, inputDb + trimDb);
    const float targetGainDb =
        effect.bypassed ? 0.0f : DynamicsCurveComponent::staticGainDb(dotInputDb, effect);

    // UI-rate detector ballistics: attack chases more reduction, release backs
    // off, so the dot can trail the curve and overshoot it on the way down.
    const auto coefficient = [](float milliseconds) {
        return 1.0f - std::exp(-1.0f / (kTimerHz * std::max(milliseconds, 0.1f) * 0.001f));
    };
    const float rate = targetGainDb < smoothedGainDb ? coefficient(effect.attackMs)
                                                     : coefficient(effect.releaseMs);
    smoothedGainDb += (targetGainDb - smoothedGainDb) * rate;

    // The gate owns the low end of the curve, so the dot has to fall with it.
    const float gateTargetDb = (hasGate && !gateEffect.bypassed)
                                   ? DynamicsCurveComponent::gateGainDb(dotInputDb, gateEffect)
                                   : 0.0f;
    const float gateRate = gateTargetDb < smoothedGateGainDb
                               ? coefficient(gateEffect.attackMs)
                               : coefficient(gateEffect.releaseMs);
    smoothedGateGainDb += (gateTargetDb - smoothedGateGainDb) * gateRate;

    if (type == pluginrack::DynamicsEffectType::Compressor) {
        const float totalGainDb = smoothedGainDb + smoothedGateGainDb;
        curve.setLivePoint(dotInputDb, dotInputDb + totalGainDb);
        gauge.setReductionDb(-totalGainDb);
    } else {
        const float totalGainDb = effect.bypassed ? 0.0f :
            DynamicsCurveComponent::gateGainDb(dotInputDb, effect);
        curve.setLivePoint(dotInputDb, dotInputDb + totalGainDb);
        gauge.setReductionDb(-totalGainDb);
    }
}

void DynamicsWindow::GainReductionGauge::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(theme::control());
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(theme::border());
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

    g.setColour(theme::dim());
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    g.drawText("gain reduction", bounds.toNearestInt().removeFromTop(18),
               juce::Justification::centred);

    const auto face = bounds.reduced(12.0f, 24.0f);
    const float radius = juce::jmin(face.getWidth() * 0.5f, face.getHeight() * 0.5f);
    if (radius < 8.0f) return;
    const auto arcBox = juce::Rectangle<float>(radius * 2.0f, radius)
                            .withCentre(face.getCentre());
    const auto centre = juce::Point<float>(arcBox.getCentreX(), arcBox.getBottom());
    const auto angleFor = [](float reductionDb) {
        return juce::MathConstants<float>::halfPi -
               (reductionDb / kMaxReductionDb) * juce::MathConstants<float>::pi;
    };

    juce::Path scale;
    scale.addCentredArc(centre.x, centre.y, radius, radius, 0.0f,
                        -juce::MathConstants<float>::halfPi,
                        juce::MathConstants<float>::halfPi, true);
    g.setColour(theme::line().withAlpha(0.6f));
    g.strokePath(scale, juce::PathStrokeType(1.5f));
    g.drawLine(centre.x - radius, centre.y, centre.x + radius, centre.y, 1.0f);

    constexpr int kTickCount = 12;
    for (int i = 0; i <= kTickCount; ++i) {
        const float angle = angleFor(kMaxReductionDb * static_cast<float>(i) /
                                     static_cast<float>(kTickCount));
        const float inner = radius - (i % 3 == 0 ? 10.0f : 6.0f);
        const juce::Point<float> tickStart(centre.x + std::sin(angle) * inner,
                                           centre.y - std::cos(angle) * inner);
        const juce::Point<float> tickEnd(centre.x + std::sin(angle) * radius,
                                         centre.y - std::cos(angle) * radius);
        g.drawLine({ tickStart, tickEnd }, i % 3 == 0 ? 1.6f : 1.0f);
    }

    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    g.setColour(theme::dim());
    for (int db = 0; db <= static_cast<int>(kMaxReductionDb); db += 6) {
        const float angle = angleFor(static_cast<float>(db));
        const juce::Point<float> label(centre.x + std::sin(angle) * (radius - 12.0f),
                                       centre.y - std::cos(angle) * (radius - 12.0f));
        g.drawText(juce::String(db),
                   juce::Rectangle<int>(juce::roundToInt(label.x) - 12,
                                        juce::roundToInt(label.y) - 5, 24, 10),
                   juce::Justification::centred);
    }

    if (reductionDb > 0.0f) {
        juce::Path active;
        active.addCentredArc(centre.x, centre.y, radius - 4.0f, radius - 4.0f, 0.0f,
                             angleFor(reductionDb), juce::MathConstants<float>::halfPi, true);
        g.setColour(theme::green());
        g.strokePath(active, juce::PathStrokeType(3.0f));
    }

    const float needleAngle = angleFor(reductionDb);
    const juce::Point<float> tip(centre.x + std::sin(needleAngle) * (radius - 22.0f),
                                 centre.y - std::cos(needleAngle) * (radius - 22.0f));
    g.setColour(theme::text());
    g.drawLine({ centre, tip }, 2.0f);
    g.setColour(theme::text());
    g.fillEllipse(centre.x - 3.0f, centre.y - 3.0f, 6.0f, 6.0f);
}
