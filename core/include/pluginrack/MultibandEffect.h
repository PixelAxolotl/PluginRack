#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pluginrack/DspMath.h"

namespace pluginrack {

enum class MultibandMode { Compress, Expand };

struct MultibandBand {
    bool bypassed = false;
    bool soloed = false;
    bool enabled = true;
    MultibandMode mode = MultibandMode::Compress;
    float thresholdDb = -18.0f;
    float ratio = 4.0f;
    float attackMs = 10.0f;
    float releaseMs = 100.0f;
    float makeupDb = 0.0f;
    float inputGainDb = 0.0f;
    float kneeDb = 0.0f;
    bool operator==(const MultibandBand&) const = default;
};

struct MultibandEffect {
    std::uint32_t id = 0;
    bool bypassed = false;
    bool autoMakeup = false;
    std::vector<MultibandBand> bands;
    std::vector<float> crossovers;
    bool operator==(const MultibandEffect&) const = default;
};

MultibandEffect makeDefaultMultibandEffect(std::uint32_t id = 1);
std::vector<MultibandEffect> defaultMultibandEffects();
float multibandStaticGainDb(float levelDb, const MultibandBand& band);

// One Linkwitz-Riley 4th-order split point. Each side is a 2nd-order
// Butterworth section (Q = 1/sqrt(2)) applied twice, so |LP| + |HP| meet at
// -6 dB, in phase, at the crossover and the pair sums to an allpass.
struct MultibandCrossover {
    dsp::Biquad lowPass;
    dsp::Biquad highPass;
};
MultibandCrossover multibandCrossoverCoefficients(float freqHz, double sampleRateHz) noexcept;

// Linear magnitude of band `bandIndex`'s split products at `freqHz`: the
// high-pass cascade of every split below the band times the band's own
// low-pass. The tree's allpass compensation is magnitude-flat, so this is the
// gain the render path applies to that frequency.
float multibandBandMagnitude(const MultibandEffect& effect, std::size_t bandIndex,
                             float freqHz, double sampleRateHz) noexcept;

class MultibandProcessor {
public:
    // Reserve every buffer the render path needs; call from the message thread.
    void prepare(double sampleRate, int maxBlockFrames = 0);
    void setEffect(const MultibandEffect& effect);
    void process(float* left, float* right, int frames) noexcept;
    void reset() noexcept;
    const std::vector<float>& gainReductionDb() const noexcept { return reductions_; }
    float autoMakeupDb() const noexcept { return autoMakeupDb_; }

private:
    struct BiquadState { float s1L = 0.0f, s2L = 0.0f, s1R = 0.0f, s2R = 0.0f; };
    struct Envelope { float env = 0.0f; float gain = 1.0f; };
    // A_x = LP4(x) + HP4(x) run in parallel: the allpass a band carries for
    // every split above it, which is what makes the LR4 tree sum flat.
    struct AllpassState { BiquadState lowA, lowB, highA, highB; };
    double sampleRate_ = 48000.0;
    MultibandEffect effect_;
    std::vector<dsp::Biquad> hpCoeffs_;   // two sections per split
    std::vector<dsp::Biquad> lpCoeffs_;   // two sections per split
    std::vector<BiquadState> hpStates_;
    std::vector<BiquadState> lpStates_;
    // Indexed [band * splits + split]; only the splits above a band are used.
    std::vector<AllpassState> apStates_;
    std::vector<Envelope> envelopes_;
    std::vector<float> reductions_;
    std::vector<float> bufL_;
    std::vector<float> bufR_;
    std::vector<float> apL_;
    std::vector<float> apR_;
    std::vector<float> outL_;
    std::vector<float> outR_;
    std::vector<float> pathL_;
    std::vector<float> pathR_;
    float autoMakeupDb_ = 0.0f;
    int activeSolo_ = -1;
    bool prepared_ = false;
    void rebuild();
    void reserveScratch(int frames);
    void applyCascadePair(float* left, float* right, int frames, const dsp::Biquad& coefficients,
                          BiquadState& first, BiquadState& second) noexcept;
};

} // namespace pluginrack
