#include "pluginrack/LimiterEffect.h"

#include "pluginrack/DspMath.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {

LimiterEffect makeDefaultLimiterEffect(std::uint32_t id) {
    LimiterEffect effect;
    effect.id = id;
    return effect;
}

std::vector<LimiterEffect> defaultLimiterEffects() { return {makeDefaultLimiterEffect(1)}; }

// Processing delay of the limiter's look-ahead line, shared by the processor
// and every latency-compensation caller (mixer strip latency, live engine taps)
// so the reported latency can never drift from the delay actually applied.
// A bypassed limiter reports 0: process() returns early and the line is bypassed.
// A non-positive or non-finite rate has no delay line yet, so it also reports 0.
int limiterLatencySamples(const LimiterEffect& effect, double sampleRate) noexcept {
    if (effect.bypassed || !(sampleRate > 0.0) || !std::isfinite(sampleRate)) {
        return 0;
    }
    const float ms = std::clamp(effect.lookaheadMs, 0.0f, 10.0f);
    return static_cast<int>(std::lround(static_cast<double>(ms) * 0.001 * sampleRate));
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
    gain_ = 1.0f; reductionDb_ = 0.0f;
    delayWrite_ = 0;
    for (auto& v : delayL_) v = 0.0f;
    for (auto& v : delayR_) v = 0.0f;
}

int LimiterProcessor::lookaheadSamples() const noexcept {
    return limiterLatencySamples(effect_, sampleRate_);
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
    if (delayCapacity_ <= 0) ensureDelay();
    const float inputGain = dsp::dbToGain(std::clamp(effect_.inputGainDb, -48.0f, 48.0f));
    const float ceilingGain = dsp::dbToGain(std::clamp(effect_.ceilingDb, -12.0f, 0.0f));
    const float release = dsp::smoothingCoeff(sampleRate_, std::clamp(effect_.releaseMs, 1.0f, 2000.0f));
    const int la = lookaheadSamples();
    const int cap = delayCapacity_;
    // The detector reads the newest sample while the output leaves through
    // the delay line, so a gain change lands exactly la samples early.
    for (int i = 0; i < frames; ++i) {
        float inL = left[i] * inputGain;
        float inR = right[i] * inputGain;
        // Sanitize before the delay ring: one NaN would otherwise circulate
        // through the look-ahead line long after the input is clean again.
        if (!std::isfinite(inL)) inL = 0.0f;
        if (!std::isfinite(inR)) inR = 0.0f;
        delayL_[static_cast<std::size_t>(delayWrite_)] = inL;
        delayR_[static_cast<std::size_t>(delayWrite_)] = inR;
        int read = delayWrite_ - la;
        if (read < 0) read += cap;
        delayWrite_ = (delayWrite_ + 1) % cap;
        const float delayedL = la > 0 ? delayL_[static_cast<std::size_t>(read)] : inL;
        const float delayedR = la > 0 ? delayR_[static_cast<std::size_t>(read)] : inR;
        const float level = std::max(std::fabs(inL), std::fabs(inR));
        const float needed = level > ceilingGain && level > 1.0e-8f ? ceilingGain / level : 1.0f;
        const float targetGain = std::min(1.0f, needed);
        // Attack instantly (lookahead gives the warning), release slowly.
        if (targetGain < gain_) gain_ = targetGain;
        else gain_ += (targetGain - gain_) * release;
        dsp::flushState(gain_);
        const float finalL = delayedL * gain_;
        const float finalR = delayedR * gain_;
        left[i] = finalL; right[i] = finalR;
        reductionDb_ = -dsp::gainToDb(gain_);
    }
}

} // namespace pluginrack
