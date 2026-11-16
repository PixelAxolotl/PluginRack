#include "pluginrack/MultibandEffect.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {
namespace {

// Band split crossovers share the Butterworth poles with every other filter.
dsp::Biquad crossoverCoeffs(float freqHz, double rate, bool lowPass) noexcept {
    return lowPass ? dsp::lowPass(freqHz, rate, dsp::kButterworthQ, 20.0)
                   : dsp::highPass(freqHz, rate, dsp::kButterworthQ, 20.0);
}

} // namespace

MultibandEffect makeDefaultMultibandEffect(std::uint32_t id) {
    MultibandEffect effect;
    effect.id = id;
    effect.bands = {MultibandBand{}, MultibandBand{}, MultibandBand{}, MultibandBand{}};
    effect.crossovers = {250.0f, 2000.0f, 6000.0f};
    return effect;
}

std::vector<MultibandEffect> defaultMultibandEffects() { return {makeDefaultMultibandEffect(1)}; }

float multibandStaticGainDb(float levelDb, const MultibandBand& band) {
    if (band.bypassed || !band.enabled) return 0.0f;
    if (band.mode == MultibandMode::Expand) {
        if (levelDb >= band.thresholdDb) return 0.0f;
        return -(band.ratio - 1.0f) * (band.thresholdDb - levelDb);
    }
    const float ratio = std::max(band.ratio, 1.0f);
    const float knee = std::max(band.kneeDb, 0.0f);
    const float distance = levelDb - band.thresholdDb;
    if (knee > 0.0f && distance > -knee * 0.5f && distance < knee * 0.5f) {
        const float compressed = distance + knee * 0.5f;
        return (1.0f / ratio - 1.0f) * compressed * compressed / (2.0f * knee);
    }
    if (distance > 0.0f) return band.thresholdDb + distance / ratio - levelDb;
    return 0.0f;
}

void MultibandProcessor::prepare(double sampleRate, int maxBlockFrames) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    rebuild();
    reserveScratch(maxBlockFrames);
    reset();
}

void MultibandProcessor::setEffect(const MultibandEffect& effect) {
    effect_ = effect;
    rebuild();
}

void MultibandProcessor::reset() noexcept {
    for (auto& st : lpStates_) st = {};
    for (auto& st : hpStates_) st = {};
    for (auto& env : envelopes_) env = {};
    autoMakeupDb_ = 0.0f;
    reductions_.assign(envelopes_.size(), 0.0f);
}

void MultibandProcessor::rebuild() {
    const int bands = static_cast<int>(effect_.bands.size());
    if (bands <= 0 || static_cast<int>(effect_.crossovers.size()) != bands - 1) { prepared_ = false; return; }
    activeSolo_ = -1;
    for (int i = 0; i < bands; ++i) {
        if (effect_.bands[static_cast<std::size_t>(i)].soloed && effect_.bands[static_cast<std::size_t>(i)].enabled) {
            if (activeSolo_ < 0) activeSolo_ = i;
        }
    }
    const std::size_t xovers = static_cast<std::size_t>(bands - 1);
    lpStates_.assign(xovers, {});
    hpStates_.assign(xovers, {});
    lpCoeffs_.resize(xovers);
    hpCoeffs_.resize(xovers);
    for (std::size_t x = 0; x < xovers; ++x) {
        const float freq = effect_.crossovers[x];
        lpCoeffs_[x] = crossoverCoeffs(freq, sampleRate_, true);
        hpCoeffs_[x] = crossoverCoeffs(freq, sampleRate_, false);
    }
    envelopes_.assign(static_cast<std::size_t>(bands), {});
    reductions_.assign(static_cast<std::size_t>(bands), 0.0f);
    prepared_ = true;
}

void MultibandProcessor::reserveScratch(int frames) {
    if (frames <= 0 || static_cast<int>(bufL_.size()) >= frames) return;
    // prepare() runs before setEffect(), when crossovers is still empty.
    // Allocate for the largest band count the validator allows (6 bands = 5
    // crossovers), or process() writes past these buffers once the real
    // effect arrives.
    constexpr std::size_t kMaxCrossovers = 5;
    const std::size_t n = static_cast<std::size_t>(frames);
    const std::size_t xovers = std::max(kMaxCrossovers,
                                        effect_.crossovers.size());
    bufL_.assign(n, 0.0f); bufR_.assign(n, 0.0f);
    outL_.assign(n, 0.0f); outR_.assign(n, 0.0f);
    lpL_.assign(n * xovers, 0.0f);
    lpR_.assign(n * xovers, 0.0f);
    hpL_.assign(n * xovers, 0.0f);
    hpR_.assign(n * xovers, 0.0f);
}

void MultibandProcessor::process(float* left, float* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0 || effect_.bypassed) return;
    const int bands = static_cast<int>(effect_.bands.size());
    if (bands <= 0 || static_cast<int>(effect_.crossovers.size()) != bands - 1) return;
    if (!prepared_ || static_cast<int>(lpStates_.size()) != bands - 1) rebuild();
    if (!prepared_) return;
    if (static_cast<int>(bufL_.size()) < frames) reserveScratch(frames);
    const int xovers = bands - 1;
    for (int x = 0; x < xovers; ++x) {
        float* pl = lpL_.data() + static_cast<std::size_t>(x) * frames;
        float* pr = lpR_.data() + static_cast<std::size_t>(x) * frames;
        float* hl = hpL_.data() + static_cast<std::size_t>(x) * frames;
        float* hr = hpR_.data() + static_cast<std::size_t>(x) * frames;
        for (int i = 0; i < frames; ++i) { pl[i] = left[i]; pr[i] = right[i]; hl[i] = left[i]; hr[i] = right[i]; }
        auto& lps = lpStates_[static_cast<std::size_t>(x)];
        auto& hps = hpStates_[static_cast<std::size_t>(x)];
        dsp::applyBiquad(pl, frames, lpCoeffs_[static_cast<std::size_t>(x)], lps.s1L, lps.s2L);
        dsp::applyBiquad(pr, frames, lpCoeffs_[static_cast<std::size_t>(x)], lps.s1R, lps.s2R);
        dsp::applyBiquad(hl, frames, hpCoeffs_[static_cast<std::size_t>(x)], hps.s1L, hps.s2L);
        dsp::applyBiquad(hr, frames, hpCoeffs_[static_cast<std::size_t>(x)], hps.s1R, hps.s2R);
    }
    std::fill_n(outL_.data(), frames, 0.0f);
    std::fill_n(outR_.data(), frames, 0.0f);
    float totalGr = 0.0f;
    int activeCount = 0;
    for (int b = 0; b < bands; ++b) {
        const auto& band = effect_.bands[static_cast<std::size_t>(b)];
        const bool muted = activeSolo_ >= 0 && activeSolo_ != b;
        for (int i = 0; i < frames; ++i) {
            float l = (b == 0) ? lpL_[static_cast<std::size_t>(i)] : hpL_[static_cast<std::size_t>(b - 1) * frames + i];
            float r = (b == 0) ? lpR_[static_cast<std::size_t>(i)] : hpR_[static_cast<std::size_t>(b - 1) * frames + i];
            if (b > 0 && b < bands - 1) {
                l -= hpL_[static_cast<std::size_t>(b) * frames + i];
                r -= hpR_[static_cast<std::size_t>(b) * frames + i];
            }
            bufL_[static_cast<std::size_t>(i)] = l;
            bufR_[static_cast<std::size_t>(i)] = r;
        }
        reductions_[static_cast<std::size_t>(b)] = 0.0f;
        if (muted) continue;
        if (band.bypassed || !band.enabled) {
            // A band that is off keeps its audio; only the dynamics stop.
            for (int i = 0; i < frames; ++i) {
                outL_[static_cast<std::size_t>(i)] += bufL_[static_cast<std::size_t>(i)];
                outR_[static_cast<std::size_t>(i)] += bufR_[static_cast<std::size_t>(i)];
            }
            continue;
        }
        const float inputGain = dsp::dbToGain(band.inputGainDb);
        const float makeup = dsp::dbToGain(band.makeupDb);
        const float attack = dsp::smoothingCoeff(sampleRate_, band.attackMs);
        const float release = dsp::smoothingCoeff(sampleRate_, std::max(band.releaseMs, 10.0f));
        auto& env = envelopes_[static_cast<std::size_t>(b)];
        float grSum = 0.0f;
        for (int i = 0; i < frames; ++i) {
            bufL_[static_cast<std::size_t>(i)] *= inputGain;
            bufR_[static_cast<std::size_t>(i)] *= inputGain;
            const float level = std::max(std::fabs(bufL_[static_cast<std::size_t>(i)]), std::fabs(bufR_[static_cast<std::size_t>(i)]));
            env.env += (level - env.env) * (level > env.env ? attack : release);
            const float targetDb = multibandStaticGainDb(dsp::gainToDb(env.env), band);
            const float targetGain = dsp::dbToGain(targetDb) * makeup;
            const float gainRate = targetGain < env.gain ? attack : release;
            env.gain += (targetGain - env.gain) * gainRate;
            grSum += -targetDb;
            bufL_[static_cast<std::size_t>(i)] *= env.gain;
            bufR_[static_cast<std::size_t>(i)] *= env.gain;
            outL_[static_cast<std::size_t>(i)] += bufL_[static_cast<std::size_t>(i)];
            outR_[static_cast<std::size_t>(i)] += bufR_[static_cast<std::size_t>(i)];
        }
        reductions_[static_cast<std::size_t>(b)] = grSum / static_cast<float>(frames);
        totalGr += reductions_[static_cast<std::size_t>(b)];
        ++activeCount;
    }
    for (int i = 0; i < frames; ++i) {
        left[i] = outL_[static_cast<std::size_t>(i)];
        right[i] = outR_[static_cast<std::size_t>(i)];
    }
    if (effect_.autoMakeup && activeCount > 0) {
        const float avgGr = totalGr / static_cast<float>(activeCount);
        const float rate = dsp::smoothingCoeff(sampleRate_, 200.0f);
        autoMakeupDb_ += (avgGr - autoMakeupDb_) * rate;
        const float g = dsp::dbToGain(autoMakeupDb_);
        for (int i = 0; i < frames; ++i) { left[i] *= g; right[i] *= g; }
    }
}

} // namespace pluginrack
