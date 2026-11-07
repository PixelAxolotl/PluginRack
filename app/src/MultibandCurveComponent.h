#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

#include <pluginrack/MultibandEffect.h>
#include <pluginrack/TraceData.h>

#include "TraceGraphComponent.h"

class MultibandCurveComponent final : public juce::Component {
public:
    static constexpr double kMinFreqHz = 20.0;
    static constexpr double kMaxFreqHz = 20000.0;
    static constexpr float kMinBandWidthHz = 20.0f;

    MultibandCurveComponent();

    void setEffect(const pluginrack::MultibandEffect& newEffect);
    const pluginrack::MultibandEffect& getEffect() const { return effect; }

    void setSpectrum(const float* magnitudeDb, int numBins, double sampleRateHz = 48000.0);
    void setSpectrumOffsetDb(float offsetDb);
    float getSpectrumOffsetDb() const { return spectrumOffsetDb; }
    void setSpectrumWeighting(SpectrumWeighting newWeighting);
    void setGainReduction(const std::vector<float>& reductionsDb);
    void setSelectedBand(int index);
    int getSelectedBand() const { return selectedBand; }

    std::function<void(const pluginrack::MultibandEffect&)> onEffectChanged;
    std::function<void()> onBandSelected;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;

    static void normaliseEffect(pluginrack::MultibandEffect& value);
    static float compressionGainDb(float inputDb, const pluginrack::MultibandBand& band);

private:
    static juce::Colour bandColour(std::size_t index);
    float freqToX(double hz) const;
    double xToFreq(float x) const;
    int bandAtX(float x) const;
    int crossoverAt(juce::Point<float> position) const;
    float dbToY(float db, juce::Rectangle<float> graph) const;
    void notifyEffectChanged();

    pluginrack::MultibandEffect effect = pluginrack::makeDefaultMultibandEffect();
    std::vector<float> spectrumDb;
    float spectrumOffsetDb = kTraceRtaDefaultOffsetDb;
    SpectrumWeighting spectrumWeighting = SpectrumWeighting::Z;
    double spectrumSampleRateHz = 48000.0;
    std::vector<float> reductionsDb;
    std::vector<float> targetTraceDb;
    std::vector<float> gainChangeTraceDb;
    int selectedBand = 0;
    int dragCrossover = -1;
    bool draggingCrossover = false;
    bool hoveringCrossover = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MultibandCurveComponent)
};
