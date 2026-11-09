#pragma once
#include <JuceHeader.h>
#include <pluginrack/DeEsserEffect.h>
#include "MiniButton.h"
#include "ValueEditSlider.h"
#include "Theme.h"
#include "TraceGraphComponent.h"
#include <pluginrack/TraceData.h>

class DeEsserWindow final : public juce::DocumentWindow, private juce::Timer {
public:
    using ApplyCallback = std::function<void(const pluginrack::DeEsserEffect&, bool bypassChanged)>;
    static constexpr int kWidth = 860;
    static constexpr int kHeight = 560;
    DeEsserWindow(const juce::String& title, const pluginrack::DeEsserEffect& initialEffect,
                  ApplyCallback apply,
                  float initialRtaOffsetDb = 0.0f,
                  int initialWeighting = 0,
                  double sampleRateHz = 48000.0);
    ~DeEsserWindow() override = default;
    void resized() override;
    void timerCallback() override;
    void setBypassed(bool b);
    void closeButtonPressed() override { if (onClose) onClose(); }
    std::function<void()> onClose;
    std::function<void(float*, float*, int)> pullAnalyzerTap;
    std::function<void(float offsetDb)> applySpectrumOffset;
    std::function<void(int weighting)> applyWeighting;
    void setAnalyzerMode(int m);
    const pluginrack::DeEsserEffect& getEffect() const { return effect; }
private:
    void setRtaOffsetDb(float offsetDb);
    void setWeightingIndex(int index);
    pluginrack::DeEsserEffect effect;
    ApplyCallback apply;
    juce::Component content;
    TraceGraphComponent graph;
    juce::TextButton enableButton{"Enable"};
    juce::Label modeLabel{"DE-ESSER", "DE-ESSER"};
    juce::TextButton modeButton{"Split"};
    juce::ComboBox presetCombo;
    ValueEditSlider freq, q, threshold, range, attack, release;
    juce::Label freqLabel{"", "Freq"}, qLabel{"", "Q"}, thresholdLabel{"", "Threshold"},
               rangeLabel{"", "Range"}, attackLabel{"", "Attack"}, releaseLabel{"", "Release"};
    static constexpr int kAnalyzerFftSize = 8192;
    std::vector<float> tapL;
    std::vector<float> tapR;
    std::unique_ptr<juce::dsp::FFT> analyzerFft;
    std::vector<float> hannCoeffs;
    std::vector<float> fftScratch;
    std::vector<float> spectrumDb;
    std::vector<float> spectrumAvg;
    juce::Label analyzerLabel{"", "Analyzer"};
    juce::Label rtaOffsetLabel{"", "0"};
    MiniButton rtaMinusButton{"-"};
    MiniButton rtaPlusButton{"+"};
    MiniButton weightingZ{"Z"};
    MiniButton weightingA{"A"};
    MiniButton weightingC{"C"};
    std::array<MiniButton*, 3> weightingButtons{&weightingZ, &weightingA, &weightingC};
    int weightingIndex = -1;
    SpectrumWeighting weighting = SpectrumWeighting::Z;
    float rtaOffsetDb = 0.0f;
    float spectrumOffsetDb = kTraceRtaDefaultOffsetDb;
    double sampleRateHz = 48000.0;
    float gainReductionDb = 0.0f;
    float detS1L = 0.0f, detS2L = 0.0f, detS1R = 0.0f, detS2R = 0.0f;
    void syncControls();
    void applyPreset(int id);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeEsserWindow)
};
