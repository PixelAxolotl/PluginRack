#include "pluginrack/DeEsserEffect.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kButterworthQ = 0.7071067811865476;
float dbToGain(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
float gainToDb(float gain) noexcept { return 20.0f * std::log10(std::max(gain, 1.0e-8f)); }
float smoothing(double rate, float ms) noexcept {
    return 1.0f - std::exp(-1.0f / (static_cast<float>(rate) * std::max(ms, 0.1f) * 0.001f));
}
void bellCoeffs(float freqHz, float q, double rate, float* c) noexcept {
    const double f = std::clamp(static_cast<double>(freqHz), 100.0, 16000.0);
    const double qq = std::clamp(static_cast<double>(q), 0.1, 20.0);
    const double w = 2.0 * kPi * f / rate;
    const double alpha = std::sin(w) / (2.0 * qq);
    // Unity-gain band extract: A = 1, so the bell peaks at 0 dB. The old
    // A=10^(12/40) boosted the band by +12 dB, which doubled the sibilance
    // instead of just passing it (peak 0.99 on a 0.5 input).
    const double A = 1.0;
    const double b0 = 1.0 + alpha * A;
    const double b1 = -2.0 * std::cos(w);
    const double b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A;
    const double a1 = -2.0 * std::cos(w);
    const double a2 = 1.0 - alpha / A;
    c[0] = static_cast<float>(b0 / a0);
    c[1] = static_cast<float>(b1 / a0);
    c[2] = static_cast<float>(b2 / a0);
    c[3] = static_cast<float>(a1 / a0);
    c[4] = static_cast<float>(a2 / a0);
}

void bandpassCoeffs(float freqHz, float q, double rate, float* c) noexcept {
    const double f = std::clamp(static_cast<double>(freqHz), 100.0, 16000.0);
    const double qq = std::clamp(static_cast<double>(q), 0.1, 20.0);
    const double w = 2.0 * kPi * f / rate;
    const double alpha = std::sin(w) / (2.0 * qq);
    const double cosW = std::cos(w);
    const double b0 = alpha;
    const double b1 = 0.0;
    const double b2 = -alpha;
    const double a0 = 1.0 + alpha;
    c[0] = static_cast<float>(b0 / a0);
    c[1] = static_cast<float>(b1 / a0);
    c[2] = static_cast<float>(b2 / a0);
    c[3] = static_cast<float>(-2.0 * cosW / a0);
    c[4] = static_cast<float>((1.0 - alpha) / a0);
}
void hpCoeffs(float freqHz, double rate, float* c) noexcept {
    const double f = std::clamp(static_cast<double>(freqHz), 100.0, 16000.0);
    const double w = 2.0 * kPi * f / rate;
    const double alpha = std::sin(w) / (2.0 * kButterworthQ);
    const double cosW = std::cos(w);
    const double b0 = (1.0 + cosW) / 2.0;
    const double b1 = -(1.0 + cosW);
    const double b2 = (1.0 + cosW) / 2.0;
    const double a0 = 1.0 + alpha;
    c[0] = static_cast<float>(b0 / a0);
    c[1] = static_cast<float>(b1 / a0);
    c[2] = static_cast<float>(b2 / a0);
    c[3] = static_cast<float>(-2.0 * cosW / a0);
    c[4] = static_cast<float>((1.0 - alpha) / a0);
}
float applyBiquadSample(float in, const float* c, float& s1, float& s2) noexcept {
    const float out = c[0] * in + s1;
    s1 = c[1] * in - c[3] * out + s2;
    s2 = c[2] * in - c[4] * out;
    return out;
}
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
        return gainToDb(peak);
    }
    float band[5];
    bandpassCoeffs(effect.freqHz, effect.q, sampleRateHz > 0.0 ? sampleRateHz : 48000.0, band);
    float peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float bandL = applyBiquadSample(left[i], band, stateS1L, stateS2L);
        const float bandR = applyBiquadSample(right[i], band, stateS1R, stateS2R);
        peak = std::max({peak, std::fabs(bandL), std::fabs(bandR)});
    }
    return gainToDb(peak);
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
    detS1L_ = detS2L_ = detS1R_ = detS2R_ = 0.0f;
}

void DeEsserProcessor::rebuild() {
    float band[5], det[5];
    // True band-pass, NOT a bell boost. The bell has 0 dB everywhere except
    // a peak — subtracting the bell from the input leaves a signal that
    // still contains most of the low frequencies, so (in - band) + band*gain
    // reduces those lows too. A band-pass contains ONLY the sibilance
    // region, so out-of-band is bit-identical when gain < 1.
    bandpassCoeffs(effect_.freqHz, effect_.q, sampleRate_, band);
    hpCoeffs(effect_.freqHz, sampleRate_, det);
    b0_ = band[0]; b1_ = band[1]; b2_ = band[2]; a1_ = band[3]; a2_ = band[4];
    detB0_ = det[0]; detB1_ = det[1]; detB2_ = det[2]; detA1_ = det[3]; detA2_ = det[4];
}

void DeEsserProcessor::process(float* left, float* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0 || effect_.bypassed) return;
    const float attack = smoothing(sampleRate_, effect_.attackMs);
    const float release = smoothing(sampleRate_, effect_.releaseMs);
    const bool split = effect_.mode == DeEsserMode::Split;
    const int n = frames;
    std::vector<float> bandL(static_cast<std::size_t>(n));
    std::vector<float> bandR(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float inL = left[i];
        const float inR = right[i];
        if (split) {
            // Band-pass at (freqHz, Q): output contains ONLY the sibilance
            // region, so out = (in - band) + band*gain leaves out-of-band
            // bit-identical. The detector reads the band directly (not a
            // separate high-pass) so what it measures is exactly what it
            // reduces. Both splits read the untouched input sample.
            const float bandC[5] = {b0_, b1_, b2_, a1_, a2_};
            bandL[static_cast<std::size_t>(i)] = applyBiquadSample(inL, bandC, s1L_, s2L_);
            bandR[static_cast<std::size_t>(i)] = applyBiquadSample(inR, bandC, s1R_, s2R_);
        } else {
            bandL[static_cast<std::size_t>(i)] = inL;
            bandR[static_cast<std::size_t>(i)] = inR;
        }
    }
    for (int i = 0; i < n; ++i) {
        const float detLevel = std::max(std::fabs(bandL[static_cast<std::size_t>(i)]),
                                        std::fabs(bandR[static_cast<std::size_t>(i)]));
        envelope_ += (detLevel - envelope_) * (detLevel > envelope_ ? attack : release);
        const float targetDb = deEsserStaticGainDb(gainToDb(envelope_), effect_);
        const float targetGain = dbToGain(targetDb);
        const float gainRate = targetGain < gain_ ? attack : release;
        gain_ += (targetGain - gain_) * gainRate;
        reductionDb_ = -targetDb;
        if (split) {
            left[i] = (left[i] - bandL[static_cast<std::size_t>(i)]) + bandL[static_cast<std::size_t>(i)] * gain_;
            right[i] = (right[i] - bandR[static_cast<std::size_t>(i)]) + bandR[static_cast<std::size_t>(i)] * gain_;
        } else {
            left[i] *= gain_;
            right[i] *= gain_;
        }
    }
}

} // namespace pluginrack
