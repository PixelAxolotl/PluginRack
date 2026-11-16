#include "pluginrack/DeEsserEffect.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {
namespace {

// The sibilance band lives inside the de-esser's own domain so a sweep cannot
// push the filter onto the vocal body or past Nyquist.
constexpr double kMinBandHz = 100.0;
constexpr double kMaxBandHz = 16000.0;

} // namespace

DeEsserEffect makeDefaultDeEsserEffect(std::uint32_t id) {
    DeEsserEffect effect;
    effect.id = id;
    return effect;
}

std::vector<DeEsserEffect> defaultDeEsserEffects() { return {makeDefaultDeEsserEffect(1)}; }

float deEsserStaticGainDb(float levelDb, const DeEsserEffect& effect) {
    if (effect.bypassed) return 0.0f;
    const float over = levelDb - effect.thresholdDb;
    if (over <= 0.0f) return 0.0f;
    return -std::min(effect.rangeDb, over);
}

float deEsserBandLevelDb(const float* left, const float* right, int frames,
                         const DeEsserEffect& effect, double sampleRateHz,
                         float& stateS1L, float& stateS2L, float& stateS1R, float& stateS2R) {
    if (left == nullptr || right == nullptr || frames <= 0) return -100.0f;
    if (effect.mode == DeEsserMode::Wideband) {
        float peak = 0.0f;
        for (int i = 0; i < frames; ++i)
            peak = std::max({peak, std::fabs(left[i]), std::fabs(right[i])});
        return dsp::gainToDb(peak);
    }
    const auto band = dsp::bandPass(effect.freqHz, sampleRateHz > 0.0 ? sampleRateHz : 48000.0,
                                    effect.q, kMinBandHz, kMaxBandHz);
    float peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float bandL = dsp::applyBiquadSample(left[i], band, stateS1L, stateS2L);
        const float bandR = dsp::applyBiquadSample(right[i], band, stateS1R, stateS2R);
        peak = std::max({peak, std::fabs(bandL), std::fabs(bandR)});
    }
    return dsp::gainToDb(peak);
}

void DeEsserProcessor::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    rebuild();
    reset();
}

void DeEsserProcessor::setEffect(const DeEsserEffect& effect) {
    effect_ = effect;
    rebuild();
}

void DeEsserProcessor::reset() noexcept {
    envelope_ = 0.0f; gain_ = 1.0f; reductionDb_ = 0.0f;
    s1L_ = s2L_ = s1R_ = s2R_ = 0.0f;
}

void DeEsserProcessor::rebuild() {
    // True band-pass, NOT a bell boost: the band contains only the sibilance
    // region, so (in - band) + band*gain leaves out-of-band bit-identical.
    band_ = dsp::bandPass(effect_.freqHz, sampleRate_, effect_.q, kMinBandHz, kMaxBandHz);
}

void DeEsserProcessor::process(float* left, float* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0 || effect_.bypassed) return;
    const float attack = dsp::smoothingCoeff(sampleRate_, effect_.attackMs);
    const float release = dsp::smoothingCoeff(sampleRate_, effect_.releaseMs);
    const bool split = effect_.mode == DeEsserMode::Split;
    for (int i = 0; i < frames; ++i) {
        const float inL = left[i];
        const float inR = right[i];
        // The detector reads the band it reduces; both splits read the
        // untouched input so the two branches stay comparable.
        const float bandL = split ? dsp::applyBiquadSample(inL, band_, s1L_, s2L_) : inL;
        const float bandR = split ? dsp::applyBiquadSample(inR, band_, s1R_, s2R_) : inR;
        const float detLevel = std::max(std::fabs(bandL), std::fabs(bandR));
        envelope_ += (detLevel - envelope_) * (detLevel > envelope_ ? attack : release);
        const float targetDb = deEsserStaticGainDb(dsp::gainToDb(envelope_), effect_);
        const float targetGain = dsp::dbToGain(targetDb);
        const float gainRate = targetGain < gain_ ? attack : release;
        gain_ += (targetGain - gain_) * gainRate;
        reductionDb_ = -targetDb;
        left[i] = split ? (inL - bandL) + bandL * gain_ : inL * gain_;
        right[i] = split ? (inR - bandR) + bandR * gain_ : inR * gain_;
    }
}

} // namespace pluginrack
