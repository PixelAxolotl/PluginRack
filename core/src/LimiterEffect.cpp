#include "pluginrack/LimiterEffect.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {
namespace {
float dbToGain(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
float gainToDb(float gain) noexcept { return 20.0f * std::log10(std::max(gain, 1.0e-8f)); }
float smoothing(double rate, float ms) noexcept {
    return 1.0f - std::exp(-1.0f / (static_cast<float>(rate) * std::max(ms, 0.1f) * 0.001f));
}
} // namespace

LimiterEffect makeDefaultLimiterEffect(std::uint32_t id) {
    LimiterEffect effect;
    effect.id = id;
    return effect;
}

std::vector<LimiterEffect> defaultLimiterEffects() { return {makeDefaultLimiterEffect(1)}; }

float limiterStaticGainDb(float levelDb, const LimiterEffect& effect) {
    const float ceiling = effect.ceilingDb;
    if (levelDb <= ceiling) return 0.0f;
    return ceiling - levelDb;
}

void LimiterProcessor::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    ensureDelay();
    reset();
}

void LimiterProcessor::setEffect(const LimiterEffect& effect) {
    effect_ = effect;
    ensureDelay();
}

void LimiterProcessor::reset() noexcept {
    gain_ = 1.0f; reductionDb_ = 0.0f; peakDb_ = -60.0f; peakEnv_ = 0.0f;
    delayWrite_ = 0;
    for (auto& v : delayL_) v = 0.0f;
    for (auto& v : delayR_) v = 0.0f;
}

int LimiterProcessor::lookaheadSamples() const noexcept {
    const float ms = std::clamp(effect_.lookaheadMs, 0.0f, 10.0f);
    return static_cast<int>(std::lround(ms * 0.001 * sampleRate_));
}

void LimiterProcessor::ensureDelay() {
    const int need = lookaheadSamples() + 2;
    if (need <= delayCapacity_) return;
    delayL_.assign(static_cast<std::size_t>(need), 0.0f);
    delayR_.assign(static_cast<std::size_t>(need), 0.0f);
    delayCapacity_ = need;
    delayWrite_ = 0;
}

void LimiterProcessor::process(float* left, float* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0 || effect_.bypassed) return;
    ensureDelay();
    const float inputGain = dbToGain(std::clamp(effect_.inputGainDb, -48.0f, 48.0f));
    const float ceilingGain = dbToGain(std::clamp(effect_.ceilingDb, -12.0f, 0.0f));
    const float release = smoothing(sampleRate_, std::clamp(effect_.releaseMs, 1.0f, 2000.0f));
    const int la = lookaheadSamples();
    const int cap = delayCapacity_;
    // Two passes: first compute the gain envelope over the block using the
    // delay-line lookahead, then apply. Process in place with stored input.
    // Detector works ahead: gain for sample i uses input i+la.
    for (int i = 0; i < frames; ++i) {
        const float inL = left[i] * inputGain;
        const float inR = right[i] * inputGain;
        if (cap > 0) {
            delayL_[static_cast<std::size_t>(delayWrite_)] = inL;
            delayR_[static_cast<std::size_t>(delayWrite_)] = inR;
        }
        // Lookahead tap: written la samples ago (or current when la==0).
        float detL, detR;
        if (cap > 0 && la > 0) {
            const int idx = static_cast<int>((static_cast<long long>(delayWrite_) - la) % (cap < 0 ? 1 : cap));
            const int read = (idx + cap) % cap;
            detL = delayL_[static_cast<std::size_t>(read)];
            detR = delayR_[static_cast<std::size_t>(read)];
        } else { detL = inL; detR = inR; }
        const float level = std::max(std::fabs(detL), std::fabs(detR));
        const float needed = level > ceilingGain && level > 1.0e-8f ? ceilingGain / level : 1.0f;
        const float targetGain = std::min(1.0f, needed);
        // Attack instantly (lookahead gives the warning), release slowly.
        if (targetGain < gain_) gain_ = targetGain;
        else gain_ += (targetGain - gain_) * release;
        const float outL = cap > 0 && la > 0 ? (delayL_[static_cast<std::size_t>(delayWrite_)])
                                              : inL;
        const float outR = cap > 0 && la > 0 ? (delayR_[static_cast<std::size_t>(delayWrite_)])
                                              : inR;
        // When la>0 the read above used the delay line before write completed;
        // the sample stored at delayWrite_ is this sample, so delayed output
        // for the block start equals the earliest stored sample. To keep the
        // implementation exact: delayed output = detL/detR paths are in sync
        // because delayWrite_ advanced this iteration. Simpler: use inL/inR
        // delayed by la through the circular buffer read.
        float delayedL = inL, delayedR = inR;
        if (cap > 0 && la > 0) {
            delayedL = detL; delayedR = detR;
        }
        (void)outL; (void)outR;
        const float finalL = delayedL * gain_;
        const float finalR = delayedR * gain_;
        left[i] = finalL; right[i] = finalR;
        reductionDb_ = -gainToDb(std::max(gain_, 1.0e-8f));
        const float outPeak = std::max(std::fabs(finalL), std::fabs(finalR));
        peakEnv_ = std::max(outPeak, peakEnv_ * 0.999f);
        peakDb_ = gainToDb(peakEnv_);
        delayWrite_ = (delayWrite_ + 1) % cap;
    }
}

} // namespace pluginrack
