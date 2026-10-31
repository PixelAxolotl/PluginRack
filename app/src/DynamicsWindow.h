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
    DynamicsWindow(const juce::String& title,
                   const pluginrack::DynamicsEffect& initialEffect,
                   std::function<void(const pluginrack::DynamicsEffect&)> apply);
    ~DynamicsWindow() override = default;
    void resized() override;
    void timerCallback() override;

    void closeButtonPressed() override {
        if (onClose) onClose();
    }

    std::function<void()> onClose;
    std::function<void(float*, float*, int)> pullAnalyzerTap;

private:
    pluginrack::DynamicsEffect effect;
    pluginrack::DynamicsEffectType type;
    std::function<void(const pluginrack::DynamicsEffect&)> apply;
    juce::Component content;
    DynamicsCurveComponent curve;
    juce::Label modeLabel{"", ""};
    juce::Label thresholdLabel{"", "Threshold"};
    juce::Label ratioLabel{"", "Ratio"};
    juce::Label attackLabel{"", "Attack"};
    juce::Label releaseLabel{"", "Release"};
    juce::Label makeupLabel{"", "Makeup"};
    juce::Label inputGainLabel{"", "Input"};
    juce::Label kneeLabel{"", "Knee"};
    MiniButton bypassButton{"Bypass"};
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
