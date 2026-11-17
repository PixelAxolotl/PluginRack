#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>
#include <pluginrack/FeedbackSuppressorEffect.h>
#include "AnalyzerConfig.h"
#include "MiniButton.h"
#include "Theme.h"
#include "TraceGraphComponent.h"
#include "ValueEditSlider.h"

// A single-bar meter that uses the same well, green-to-amber gradient, peak
// block and held-peak line as the mixer's LevelMeter, captioned for the
// feedback suppressor's input or output.
class FeedbackSuppressorMeter final : public juce::Component {
public:
    explicit FeedbackSuppressorMeter(juce::String captionText);
    void setLevels(float peakLinear, float rmsLinear);
    void paint(juce::Graphics& g) override;
private:
    juce::String caption;
    float peak = 0.0f;
    float rms = 0.0f;
    float held = 0.0f;
    juce::uint32 heldAt = 0;
};

// Live RTA plus response plot. The RTA is the suppressor's processed output,
// so active cuts are visible in the trace. The response curve includes the
// detected notches and the detector HPF/LPF roll-off.
class FeedbackSuppressorGraph final : public juce::Component {
public:
    void setNotches(const float* freqs, const float* depthsDb, int count);
    void setSpectrum(const std::vector<float>& values);
    void setSpectrumOffsetDb(float offsetDb);
    void setWeighting(SpectrumWeighting weighting);
    void setInputSensitivityDb(float db);
    void setDetectorBand(float highPassHz, float lowPassHz);
    void setSampleRate(double sampleRateHz);
    void paint(juce::Graphics& g) override;
private:
    std::array<float, 8> notchFreqs{};
    std::array<float, 8> notchDepthsDb{};
    int notchCount = 0;
    std::vector<float> spectrum;
    float spectrumOffsetDb = kTraceRtaDefaultOffsetDb;
    SpectrumWeighting weighting = SpectrumWeighting::Z;
    float inputSensitivityDb = pluginrack::kFeedbackSuppressorDefaultSensitivityDb;
    float detectorHighPassHz = pluginrack::kFeedbackSuppressorDefaultHighPassHz;
    float detectorLowPassHz = pluginrack::kFeedbackSuppressorDefaultLowPassHz;
    double sampleRateHz = 48000.0;
};

class FeedbackSuppressorWindow final : public juce::DocumentWindow, private juce::Timer {
public:
    using ApplyCallback = std::function<void(const pluginrack::FeedbackSuppressorEffect&, bool bypassChanged)>;
    static constexpr int kWidth = 560;
    static constexpr int kHeight = 560;
    FeedbackSuppressorWindow(const juce::String& title,
                             const pluginrack::FeedbackSuppressorEffect& initialEffect,
                             ApplyCallback apply,
                             float initialRtaOffsetDb = 0.0f,
                             int initialWeighting = 0,
                             double sampleRateHz = 48000.0);
    ~FeedbackSuppressorWindow() override = default;
    void resized() override;
    void timerCallback() override;
    void setBypassed(bool b);
    void closeButtonPressed() override { if (onClose) onClose(); }
    std::function<void()> onClose;
    std::function<void(bool postEq, float*, float*, int)> pullAnalyzerTap;
    std::function<int(float*, float*, int)> pullNotches;
    std::function<void(float offsetDb)> applySpectrumOffset;
    std::function<void(int weighting)> applyWeighting;
    const pluginrack::FeedbackSuppressorEffect& getEffect() const { return effect; }
private:
    pluginrack::FeedbackSuppressorEffect effect;
    ApplyCallback apply;
    juce::Component content;
    juce::Label modeLabel{"FEEDBACK SUPPRESSOR", "FEEDBACK SUPPRESSOR"};
    juce::TextButton enableButton{"Enable"};
    ValueEditSlider strength;
    juce::Label strengthLabel{"", "Cuts"};
    ValueEditSlider attenuation;
    juce::Label attenuationLabel{"", "Attenuation"};
    ValueEditSlider gainReductionMultiplier;
    juce::Label gainReductionMultiplierLabel{"", "GR Mult"};
    ValueEditSlider sensitivity;
    juce::Label sensitivityLabel{"", "Input Sens"};
    ValueEditSlider highPass;
    juce::Label highPassLabel{"", "Detector HPF"};
    ValueEditSlider lowPass;
    juce::Label lowPassLabel{"", "Detector LPF"};
    ValueEditSlider attack;
    juce::Label attackLabel{"", "Attack"};
    ValueEditSlider hold;
    juce::Label holdLabel{"", "Hold"};
    ValueEditSlider release;
    juce::Label releaseLabel{"", "Release"};
    ValueEditSlider detectionDelay;
    juce::Label detectionDelayLabel{"", "Confirm"};
    FeedbackSuppressorGraph graph;
    FeedbackSuppressorMeter inputMeter{"IN"};
    FeedbackSuppressorMeter outputMeter{"OUT"};
    juce::Label infoLabel;
    juce::Label analyzerLabel{"", "Analyzer"};
    juce::Label rtaOffsetLabel{"", "0"};
    MiniButton rtaMinusButton{"-"};
    MiniButton rtaPlusButton{"+"};
    MiniButton weightingZ{"Z"};
    MiniButton weightingA{"A"};
    MiniButton weightingC{"C"};
    std::array<MiniButton*, 3> weightingButtons{&weightingZ, &weightingA, &weightingC};
    std::array<float, 8> lastNotches{};
    std::array<float, 8> lastDepths{};
    int lastNotchCount = 0;
    int weightingIndex = -1;
    SpectrumWeighting weighting = SpectrumWeighting::Z;
    float rtaOffsetDb = 0.0f;
    float spectrumOffsetDb = kTraceRtaDefaultOffsetDb;
    double sampleRateHz = 48000.0;
    static constexpr int kAnalyzerFftSize = analyzer::kFftSize;
    std::vector<float> tapL;
    std::vector<float> tapR;
    std::vector<float> outL;
    std::vector<float> outR;
    std::unique_ptr<juce::dsp::FFT> analyzerFft;
    std::vector<float> hannCoeffs;
    std::vector<float> fftScratch;
    std::vector<float> spectrumDb;
    std::vector<float> spectrumAvg;
    void syncControls();
    void setRtaOffsetDb(float offsetDb);
    void setWeightingIndex(int index);
    void styleKnob(ValueEditSlider& slider, double min, double max, double step,
                   const juce::String& suffix, double resetValue, juce::Colour fill);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FeedbackSuppressorWindow)
};
