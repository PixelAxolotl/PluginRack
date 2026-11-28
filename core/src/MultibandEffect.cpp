#include "pluginrack/MultibandEffect.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {

MultibandEffect makeDefaultMultibandEffect(std::uint32_t id) {
    MultibandEffect effect;
    effect.id = id;
    effect.bands = {MultibandBand{}, MultibandBand{}, MultibandBand{}, MultibandBand{}};
    effect.crossovers = {250.0f, 2000.0f, 6000.0f};
    return effect;
}

std::vector<MultibandEffect> defaultMultibandEffects() { return {makeDefaultMultibandEffect(1)}; }

// Reference curve (UI, tests, and the editor's graph). The render path in
// MultibandProcessor::process() evaluates the same law in the gain domain so a
// sample needs at most one pow; any change here must be mirrored there.
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

MultibandCrossover multibandCrossoverCoefficients(float freqHz, double sampleRateHz) noexcept {
    const double rate = sampleRateHz > 0.0 ? sampleRateHz : 48000.0;
    MultibandCrossover split;
    split.lowPass = dsp::lowPass(freqHz, rate, dsp::kButterworthQ, 20.0);
    split.highPass = dsp::highPass(freqHz, rate, dsp::kButterworthQ, 20.0);
    return split;
}

float multibandBandMagnitude(const MultibandEffect& effect, std::size_t bandIndex,
                             float freqHz, double sampleRateHz) noexcept {
    const std::size_t bands = effect.bands.size();
    if (bands == 0 || bandIndex >= bands || effect.crossovers.size() != bands - 1) return 0.0f;
    const double rate = sampleRateHz > 0.0 ? sampleRateHz : 48000.0;
    double magnitude = 1.0;
    for (std::size_t x = 0; x < bandIndex; ++x) {
        const auto split = multibandCrossoverCoefficients(effect.crossovers[x], rate);
        const auto high = static_cast<double>(dsp::biquadMagnitude(split.highPass, freqHz, rate));
        magnitude *= high * high;
    }
    if (bandIndex + 1 < bands) {
        const auto split = multibandCrossoverCoefficients(effect.crossovers[bandIndex], rate);
        const auto low = static_cast<double>(dsp::biquadMagnitude(split.lowPass, freqHz, rate));
        magnitude *= low * low;
    }
    return static_cast<float>(magnitude);
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
    for (auto& st : hpStates_) st = {};
    for (auto& st : lpStates_) st = {};
    for (auto& st : apStates_) st = {};
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
    // Linkwitz-Riley 4th order: each split is a 2nd-order Butterworth section
    // run twice, so LP4(f) + HP4(f) is an allpass. The bands are the products
    // of the paths (band b = HP0..HP(b-1) * LP_b), and every band also carries
    // A_x = LP4(x) + HP4(x) for each split x above it, since a plain product
    // tree is only flat at the split itself: without that compensation two
    // bands 1.6 octaves apart sum ~1.3 dB down between them.
    const std::size_t splits = static_cast<std::size_t>(bands - 1);
    hpCoeffs_.assign(splits * 2, dsp::Biquad{});
    lpCoeffs_.assign(splits * 2, dsp::Biquad{});
    hpStates_.assign(splits * 2, {});
    lpStates_.assign(splits * 2, {});
    apStates_.assign(static_cast<std::size_t>(bands) * splits, {});
    for (std::size_t x = 0; x < splits; ++x) {
        const auto split = multibandCrossoverCoefficients(effect_.crossovers[x], sampleRate_);
        lpCoeffs_[x * 2] = split.lowPass;
        lpCoeffs_[x * 2 + 1] = split.lowPass;
        hpCoeffs_[x * 2] = split.highPass;
        hpCoeffs_[x * 2 + 1] = split.highPass;
    }
    envelopes_.assign(static_cast<std::size_t>(bands), {});
    reductions_.assign(static_cast<std::size_t>(bands), 0.0f);
    prepared_ = true;
}

void MultibandProcessor::reserveScratch(int frames) {
    if (frames <= 0) return;
    // prepare() runs before setEffect(), when crossovers is still empty. The
    // LR4 tree needs a fixed set of planes whatever the band count (the path
    // remainder, one band buffer, one allpass buffer and the output), so
    // sizing by frames alone covers the six-band, five-crossover maximum and
    // anything wider the validator would accept.
    const std::size_t n = std::max(static_cast<std::size_t>(frames),
                                   bufL_.size());
    // Both dimensions are checked before skipping: a longer block needs wider
    // rows even when the plane count already fits.
    if (bufL_.size() >= n && pathL_.size() >= n && apL_.size() >= n) return;
    bufL_.assign(n, 0.0f); bufR_.assign(n, 0.0f);
    apL_.assign(n, 0.0f); apR_.assign(n, 0.0f);
    outL_.assign(n, 0.0f); outR_.assign(n, 0.0f);
    pathL_.assign(n, 0.0f); pathR_.assign(n, 0.0f);
}

void MultibandProcessor::applyCascadePair(float* left, float* right, int frames,
                                          const dsp::Biquad& coefficients,
                                          BiquadState& first, BiquadState& second) noexcept {
    dsp::applyBiquad(left, frames, coefficients, first.s1L, first.s2L);
    dsp::applyBiquad(right, frames, coefficients, first.s1R, first.s2R);
    dsp::applyBiquad(left, frames, coefficients, second.s1L, second.s2L);
    dsp::applyBiquad(right, frames, coefficients, second.s1R, second.s2R);
}

void MultibandProcessor::process(float* left, float* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0 || effect_.bypassed) return;
    const int bands = static_cast<int>(effect_.bands.size());
    if (bands <= 0 || static_cast<int>(effect_.crossovers.size()) != bands - 1) return;
    if (!prepared_ || static_cast<int>(hpStates_.size()) != (bands - 1) * 2) rebuild();
    if (!prepared_) return;
    if (static_cast<int>(bufL_.size()) < frames
        || static_cast<int>(pathL_.size()) < frames
        || static_cast<int>(apL_.size()) < frames) {
        reserveScratch(frames);
    }
    // A single NaN/Inf sample must not poison the split or the envelopes for
    // the rest of the session: silence non-finite input here and every buffer
    // downstream stays finite.
    for (int i = 0; i < frames; ++i) {
        if (!std::isfinite(left[i])) left[i] = 0.0f;
        if (!std::isfinite(right[i])) right[i] = 0.0f;
    }
    for (int i = 0; i < frames; ++i) {
        pathL_[static_cast<std::size_t>(i)] = left[i];
        pathR_[static_cast<std::size_t>(i)] = right[i];
    }
    std::fill_n(outL_.data(), frames, 0.0f);
    std::fill_n(outR_.data(), frames, 0.0f);
    float totalGr = 0.0f;
    int activeCount = 0;
    const int splits = bands - 1;
    // Replace the running path with its high-pass, so the next band splits
    // what this split left over.
    const auto advancePath = [this, frames, splits](int b) {
        if (b >= splits) return;
        const auto index = static_cast<std::size_t>(b) * 2;
        applyCascadePair(pathL_.data(), pathR_.data(), frames,
                         hpCoeffs_[index], hpStates_[index], hpStates_[index + 1]);
    };
    for (int b = 0; b < bands; ++b) {
        const auto& band = effect_.bands[static_cast<std::size_t>(b)];
        const bool muted = activeSolo_ >= 0 && activeSolo_ != b;
        // The band core is the running high-pass path's low-pass; the last
        // band has no low-pass and takes the path itself.
        std::copy_n(pathL_.data(), frames, bufL_.data());
        std::copy_n(pathR_.data(), frames, bufR_.data());
        if (b < splits) {
            const auto index = static_cast<std::size_t>(b) * 2;
            applyCascadePair(bufL_.data(), bufR_.data(), frames,
                             lpCoeffs_[index], lpStates_[index], lpStates_[index + 1]);
        }
        // Allpass compensation for every split above this band. A_x is
        // LP4(x) and HP4(x) in parallel, summed.
        for (int x = b + 1; x < splits; ++x) {
            auto& state = apStates_[static_cast<std::size_t>(b) * static_cast<std::size_t>(splits)
                                    + static_cast<std::size_t>(x)];
            std::copy_n(bufL_.data(), frames, apL_.data());
            std::copy_n(bufR_.data(), frames, apR_.data());
            const auto index = static_cast<std::size_t>(x) * 2;
            applyCascadePair(bufL_.data(), bufR_.data(), frames,
                             lpCoeffs_[index], state.lowA, state.lowB);
            applyCascadePair(apL_.data(), apR_.data(), frames,
                             hpCoeffs_[index], state.highA, state.highB);
            for (int i = 0; i < frames; ++i) {
                bufL_[static_cast<std::size_t>(i)] += apL_[static_cast<std::size_t>(i)];
                bufR_[static_cast<std::size_t>(i)] += apR_[static_cast<std::size_t>(i)];
            }
        }
        reductions_[static_cast<std::size_t>(b)] = 0.0f;
        if (muted) {
            // Skipped bands still advance the tree so the ones after them see
            // the right path.
            advancePath(b);
            continue;
        }
        if (band.bypassed || !band.enabled) {
            // A band that is off keeps its audio; only the dynamics stop.
            for (int i = 0; i < frames; ++i) {
                outL_[static_cast<std::size_t>(i)] += bufL_[static_cast<std::size_t>(i)];
                outR_[static_cast<std::size_t>(i)] += bufR_[static_cast<std::size_t>(i)];
            }
            advancePath(b);
            continue;
        }
        const float inputGain = dsp::dbToGain(band.inputGainDb);
        const float makeup = dsp::dbToGain(band.makeupDb);
        const float attack = dsp::smoothingCoeff(sampleRate_, band.attackMs);
        const float release = dsp::smoothingCoeff(sampleRate_, std::max(band.releaseMs, 10.0f));
        // Gain-domain knee edges, hoisted like the single-band processor's:
        // the same curve as multibandStaticGainDb, with the soft knee left in
        // dB and every other branch a plain gain or one pow.
        const bool expand = band.mode == MultibandMode::Expand;
        const float knee = std::max(band.kneeDb, 0.0f);
        const float halfKnee = knee * 0.5f;
        const float thresholdDb = band.thresholdDb;
        const float thresholdGain = dsp::dbToGain(thresholdDb);
        const float lowKneeGain = dsp::dbToGain(thresholdDb - halfKnee);
        const float highKneeGain = dsp::dbToGain(thresholdDb + halfKnee);
        const float compSlope = 1.0f / std::max(band.ratio, 1.0f) - 1.0f;
        const float expandSlope = band.ratio - 1.0f;
        auto& env = envelopes_[static_cast<std::size_t>(b)];
        float grSum = 0.0f;
        for (int i = 0; i < frames; ++i) {
            bufL_[static_cast<std::size_t>(i)] *= inputGain;
            bufR_[static_cast<std::size_t>(i)] *= inputGain;
            const float level = std::max(std::fabs(bufL_[static_cast<std::size_t>(i)]), std::fabs(bufR_[static_cast<std::size_t>(i)]));
            env.env += (level - env.env) * (level > env.env ? attack : release);
            float staticGain = 1.0f;
            if (expand) {
                // Below the threshold the expander drops by
                // (ratio - 1) * (thresholdDb - levelDb) dB, which in the gain
                // domain is (envelope/threshold)^(ratio - 1).
                if (env.env < thresholdGain) {
                    staticGain = std::pow(env.env / thresholdGain, expandSlope);
                }
            } else if (knee > 0.0f && env.env > lowKneeGain && env.env < highKneeGain) {
                const float distance = dsp::gainToDb(env.env) - thresholdDb;
                const float compressed = distance + halfKnee;
                staticGain = dsp::dbToGain(compSlope * compressed * compressed / (2.0f * knee));
            } else if (env.env > thresholdGain) {
                staticGain = std::pow(env.env / thresholdGain, compSlope);
            }
            const float targetGain = staticGain * makeup;
            const float gainRate = targetGain < env.gain ? attack : release;
            env.gain += (targetGain - env.gain) * gainRate;
            // Flush so a NaN parameter or a stray non-finite value cannot keep
            // the envelope poisoned after the input is clean again.
            dsp::flushState(env.env);
            dsp::flushState(env.gain);
            // A sample the curve left alone contributes exactly 0 dB, so the
            // meter only pays for the conversion on samples that moved.
            grSum += staticGain == 1.0f ? 0.0f : -dsp::gainToDb(staticGain);
            bufL_[static_cast<std::size_t>(i)] *= env.gain;
            bufR_[static_cast<std::size_t>(i)] *= env.gain;
            outL_[static_cast<std::size_t>(i)] += bufL_[static_cast<std::size_t>(i)];
            outR_[static_cast<std::size_t>(i)] += bufR_[static_cast<std::size_t>(i)];
        }
        reductions_[static_cast<std::size_t>(b)] = grSum / static_cast<float>(frames);
        totalGr += reductions_[static_cast<std::size_t>(b)];
        ++activeCount;
        advancePath(b);
    }
    for (int i = 0; i < frames; ++i) {
        left[i] = outL_[static_cast<std::size_t>(i)];
        right[i] = outR_[static_cast<std::size_t>(i)];
    }
    if (effect_.autoMakeup && activeCount > 0) {
        const float avgGr = totalGr / static_cast<float>(activeCount);
        const float rate = dsp::smoothingCoeff(sampleRate_, 200.0f);
        autoMakeupDb_ += (avgGr - autoMakeupDb_) * rate;
        dsp::flushState(autoMakeupDb_);
        const float g = dsp::dbToGain(autoMakeupDb_);
        for (int i = 0; i < frames; ++i) { left[i] *= g; right[i] *= g; }
    }
}

} // namespace pluginrack
