#pragma once

#include <JuceHeader.h>

#include <functional>
#include <utility>
#include <array>

#include <pluginrack/DynamicsEffect.h>

#include "DynamicsCurveComponent.h"
#include "MiniButton.h"
#include "Theme.h"

class DynamicsWindow final : public juce::DocumentWindow, private juce::Timer {
public:
    // `apply` gets the edited effect plus whether the bypass switch itself
    // moved; a parameter-only apply must leave the strip's bypass alone.
    using ApplyCallback = std::function<void(const pluginrack::DynamicsEffect&, bool bypassChanged)>;

    // Below these the knob rows collide with the graph, so the bay gives the
    // panel at least this much room (the EQ page does the same).
    static constexpr int kMinWidth = 820;
    static constexpr int kMinHeight = 520;

    DynamicsWindow(const juce::String& title,
                   const pluginrack::DynamicsEffect& initialEffect,
                   ApplyCallback apply,
                   // Optional channel gate, shown as a second knob row on the
                   // compressor panel and drawn under its curve.
                   const pluginrack::DynamicsEffect* gate = nullptr);
    ~DynamicsWindow() override = default;
    void resized() override;
    void timerCallback() override;

    // Mirrors a bypass change made on the strip so the panel cannot re-apply a
    // stale switch state.
    void setBypassed(bool bypassed);
    // Keeps the gate row (and its curve) in step with the strip.
    void setGate(const pluginrack::DynamicsEffect& newGate);
    pluginrack::DynamicsEffectType getType() const { return type; }

    void closeButtonPressed() override {
        if (onClose) onClose();
    }

    std::function<void()> onClose;
    std::function<void(float*, float*, int)> pullAnalyzerTap;

private:
    // Half-circle gain reduction meter; 0 dB at rest on the right, kMax on the left.
    class GainReductionGauge final : public juce::Component {
    public:
        void setReductionDb(float reduction) {
            const float clamped = juce::jlimit(0.0f, kMaxReductionDb, reduction);
            if (juce::approximatelyEqual(clamped, reductionDb)) return;
            reductionDb = clamped;
            repaint();
        }
        void paint(juce::Graphics& g) override;

    private:
        static constexpr float kMaxReductionDb = 24.0f;
        float reductionDb = 0.0f;
    };

    void applyPreset(int itemId);
    void applyGatePreset(int itemId);
    void syncControls();
    void syncGateControls();

    pluginrack::DynamicsEffect effect;
    pluginrack::DynamicsEffectType type;
    ApplyCallback apply;
    // Gate row (compressor panel only)
    bool hasGate = false;
    pluginrack::DynamicsEffect gateEffect;
    juce::Label gateCaption{"", "GATE"};
    // The gate is bypassed by default and the strip's chain row is its only
    // other switch, so the panel needs its own enable control: without one the
    // gate knobs change a processor that never runs.
    juce::TextButton gateEnableButton{"Enable"};
    juce::Label gateThresholdLabel{"", "Threshold"};
    juce::Label gateRangeLabel{"", "Range"};
    juce::Label gateAttackLabel{"", "Attack"};
    juce::Label gateReleaseLabel{"", "Release"};
    juce::Slider gateThreshold;
    juce::Slider gateRange;
    juce::Slider gateAttack;
    juce::Slider gateRelease;

    // Simulated detector gain (<= 0 dB, makeup excluded) so the dot lags the
    // curve through attack and overshoots it on release.
    float smoothedGainDb = 0.0f;
    float smoothedGateGainDb = 0.0f;
    juce::Component content;
    DynamicsCurveComponent curve;
    GainReductionGauge gauge;
    juce::Label modeLabel{"", ""};
    juce::Label thresholdLabel{"", "Threshold"};
    juce::Label ratioLabel{"", "Ratio"};
    juce::Label attackLabel{"", "Attack"};
    juce::Label releaseLabel{"", "Release"};
    juce::Label makeupLabel{"", "Output"};
    juce::Label inputGainLabel{"", "Input"};
    juce::Label kneeLabel{"", "Knee"};
    juce::ComboBox presetCombo;
    juce::ComboBox gatePresetCombo;
    juce::Slider threshold;
    juce::Slider ratio;
    juce::Slider attack;
    juce::Slider release;
    juce::Slider makeup;
    juce::Slider inputGain;
    juce::Slider knee;
    std::array<float, 512> tapL{};
    std::array<float, 512> tapR{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DynamicsWindow)
};
