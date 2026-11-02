#pragma once

#include <JuceHeader.h>

#include <juce_dsp/juce_dsp.h>

#include <memory>
#include <vector>

#include <pluginrack/DynamicsEffect.h>
#include <pluginrack/EqEngine.h>

#include "EQCurveComponent.h"
#include "Theme.h"

// Read-only thumbnail of one rack insert, drawn inside its card: the built-in
// EQ shows its curve plus the live spectrum, Compressor and Gate show their
// transfer curve plus a gain-reduction bar, a VST shows a snapshot of its live
// editor. Nothing here takes input - the center bay stays the only surface
// that edits a plugin.
class InsertMiniView final : public juce::Component {
public:
    static constexpr int kHeight = 80;

    enum class Kind { None, Eq, Dynamics, Vst };

    InsertMiniView();

    void showEq(const std::vector<pluginrack::EqBand>& bands, double sampleRateHz);
    void showDynamics(const pluginrack::DynamicsEffect& effect);
    // The dynamics pair shares one card: compressor and gate on one graph.
    void showDynamicsPair(const pluginrack::DynamicsEffect& compressor,
                          const pluginrack::DynamicsEffect& gate);
    void showVst();

    Kind getKind() const { return kind; }
    void setVstSnapshot(const juce::Image& snapshot);
    void setSpectrum(const float* magnitudesDb, int numBins);
    // Live state, pushed every tick: the panel edits the engine while the card
    // stays on screen, so a thumbnail set once at build time goes stale.
    void setEqBands(const std::vector<pluginrack::EqBand>& bands, double sampleRateHz);
    // Saved RTA display offset (0 == the default view), added to the curve's
    // base offset so the card's analyzer matches the editor's exactly.
    void setSpectrumOffsetDb(float storedDb);
    // Saved analyzer weighting index (0 = Z, 1 = A, 2 = C), the same value the
    // editor uses.
    void setWeighting(int index);
    void setDynamics(const pluginrack::DynamicsEffect& effect);
    void setDynamicsPair(const pluginrack::DynamicsEffect& compressor,
                         const pluginrack::DynamicsEffect& gate);
    // gainDb is the detector's gain (<= 0 dB); the dot and the bar both follow
    // it, so the card reads like the panel it stands for.
    void setLiveGain(float inputDb, float gainDb);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void paintVst(juce::Graphics& g);
    void paintDynamics(juce::Graphics& g);

    Kind kind = Kind::None;
    std::unique_ptr<EQCurveComponent> eqCurve;
    pluginrack::DynamicsEffect dynamics;
    pluginrack::DynamicsEffect gateEffect;
    bool hasGate = false;
    juce::Image vstSnapshot;
    float inputDb = -60.0f;
    float gainDb = 0.0f;
    bool hasLive = false;
    float spectrumOffsetDb = 0.0f;
    int weightingIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InsertMiniView)
};

// RTA feed for the mini EQ: the same Hann window, normalisation and
// attack/release ballistics as EQWindow's timer, which keeps the canonical
// implementation. Separate so the working EQ editor is not refactored for a
// thumbnail.
class MiniSpectrum {
public:
    static constexpr int kFftSize = 8192;

    MiniSpectrum();

    // left/right hold kFftSize frames, oldest first.
    void refresh(const float* left, const float* right);
    const float* magnitudes() const { return smoothed.data(); }
    int numBins() const { return static_cast<int>(smoothed.size()); }

private:
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> hann;
    std::vector<float> scratch;
    std::vector<float> fresh;
    std::vector<float> smoothed;
};

// UI-rate stand-in for a real gain-reduction tap: derives the detector gain
// from the analysed peak with the ballistics DynamicsWindow uses.
struct DynamicsMeterSim {
    float inputDb = -60.0f;
    float gainDb = 0.0f; // <= 0 dB

    void update(const float* left, const float* right, int frames,
                const pluginrack::DynamicsEffect& effect, double timerHz);
};
