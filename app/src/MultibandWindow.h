#pragma once

#include <JuceHeader.h>
#include <pluginrack/MultibandEffect.h>
#include "ValueEditSlider.h"
#include "MultibandCurveComponent.h"
#include "MiniButton.h"
#include "Theme.h"
#include "TraceGraphComponent.h"

class MultibandWindow final : public juce::DocumentWindow, private juce::Timer {
public:
    using ApplyCallback = std::function<void(const pluginrack::MultibandEffect&, bool bypassChanged)>;
    static constexpr int kWidth = 860;
    static constexpr int kHeight = 560;
    MultibandWindow(const juce::String& title,
                    const pluginrack::MultibandEffect& initialEffect,
                    ApplyCallback apply,
                    float initialRtaOffsetDb = 0.0f,
                    int initialWeighting = 0,
                    double sampleRateHz = 48000.0);
    ~MultibandWindow() override = default;
    void resized() override;
    void timerCallback() override;
    void setBypassed(bool bypassed);
    void closeButtonPressed() override { if (onClose) onClose(); }
    std::function<void()> onClose;
    std::function<void(float*, float*, int)> pullAnalyzerTap;
    std::function<void(float offsetDb)> applySpectrumOffset;
    std::function<void(int weighting)> applyWeighting;
    void setAnalyzerMode(int mode);
    const pluginrack::MultibandEffect& getEffect() const { return effect; }
    const std::vector<float>& getTapL() const { return tapL; }
    const std::vector<float>& getTapR() const { return tapR; }

private:
    void syncControls();
    void setRtaOffsetDb(float offsetDb);
    void setWeightingIndex(int index);
    pluginrack::MultibandEffect effect;
    ApplyCallback apply;
    juce::Component content;
    MultibandCurveComponent curve;
    juce::TextButton enableButton{"Enable"};
    juce::Label modeLabel{"MULTIBAND", "MULTIBAND"};
    juce::ComboBox presetCombo;
    juce::Label bandLabel{"", "Band"};
    ValueEditSlider threshold, ratio, attack, release, knee, makeup, inputGain;
    juce::Label thresholdLabel{"", "Threshold"}, ratioLabel{"", "Ratio"},
               attackLabel{"", "Attack"}, releaseLabel{"", "Release"},
               kneeLabel{"", "Knee"}, makeupLabel{"", "Output"}, inputGainLabel{"", "Input"};
    juce::TextButton modeButton{"Compress"};
    juce::TextButton soloButton{"Solo"};
    juce::TextButton muteButton{"Mute"};
    juce::TextButton addBandButton{"+ Band"};
    juce::TextButton removeBandButton{"- Band"};
    juce::TextButton autoMakeupButton{"Auto Makeup"};
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
    int selectedBand = 0;
    float liveGr = 0.0f;
    static constexpr int kAnalyzerFftSize = 8192;
    std::vector<float> tapL;
    std::vector<float> tapR;
    std::unique_ptr<juce::dsp::FFT> analyzerFft;
    std::vector<float> hannCoeffs;
    std::vector<float> fftScratch;
    std::vector<float> spectrumDb;
    std::vector<float> spectrumAvg;
    std::vector<float> gainReductionDb;
    void selectBand(int i);
    void applyPreset(int id);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MultibandWindow)
};
