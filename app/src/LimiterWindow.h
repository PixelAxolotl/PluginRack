#pragma once
#include <JuceHeader.h>
#include <pluginrack/LimiterEffect.h>
#include "ValueEditSlider.h"
#include "Theme.h"
#include "TraceGraphComponent.h"
#include <pluginrack/TraceData.h>

class LimiterWindow final : public juce::DocumentWindow, private juce::Timer {
public:
    using ApplyCallback = std::function<void(const pluginrack::LimiterEffect&, bool bypassChanged)>;
    static constexpr int kWidth = 860;
    static constexpr int kHeight = 560;
    LimiterWindow(const juce::String& title, const pluginrack::LimiterEffect& initialEffect, ApplyCallback apply);
    ~LimiterWindow() override = default;
    void resized() override;
    void timerCallback() override;
    void setBypassed(bool b);
    void closeButtonPressed() override { if (onClose) onClose(); }
    std::function<void()> onClose;
    std::function<void(bool postEq, float*, float*, int)> pullAnalyzerTap;
    void setAnalyzerMode(int m);
    const pluginrack::LimiterEffect& getEffect() const { return effect; }
    float getGrDb() const { return grDb; } float getPeakDb() const { return peakDb; }
private:
    pluginrack::LimiterEffect effect;
    ApplyCallback apply;
    juce::Component content;
    TraceGraphComponent graph;
    juce::TextButton enableButton{"Enable"};
    juce::Label modeLabel{"", "LIMITER"};
    juce::ComboBox presetCombo;
    ValueEditSlider inputGain, ceiling, lookahead, release;
    juce::Label inputGainLabel{"", "Input Gain"}, ceilingLabel{"", "Ceiling"},
               lookaheadLabel{"", "Lookahead"}, releaseLabel{"", "Release"};
    std::array<float, 2048> preTapL{};
    std::array<float, 2048> preTapR{};
    std::array<float, 2048> postTapL{};
    std::array<float, 2048> postTapR{};
    pluginrack::LevelHistory levelHistory{240};
    std::vector<float> historyInput;
    std::vector<float> historyReduction;
    std::vector<float> historyOutput;
    void syncControls();
    void applyPreset(int id);
    float grDb = 0.0f, peakDb = -60.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LimiterWindow)
};
