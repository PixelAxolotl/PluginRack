#include "pluginrack/MultibandEffect.h"

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
void lrCoeffs(float freqHz, double rate, bool lowPass, float* c) noexcept {
    const double f = std::clamp(static_cast<double>(freqHz), 20.0, 20000.0);
    const double w = 2.0 * kPi * f / rate;
    const double alpha = std::sin(w) / (2.0 * kButterworthQ);
    const double cosW = std::cos(w);
    double b0, b1, b2;
    if (lowPass) { b0 = (1.0 - cosW) / 2.0; b1 = 1.0 - cosW; b2 = (1.0 - cosW) / 2.0; }
    else { b0 = (1.0 + cosW) / 2.0; b1 = -(1.0 + cosW); b2 = (1.0 + cosW) / 2.0; }
    const double a0 = 1.0 + alpha;
    c[0] = static_cast<float>(b0 / a0);
    c[1] = static_cast<float>(b1 / a0);
    c[2] = static_cast<float>(b2 / a0);
    c[3] = static_cast<float>(-2.0 * cosW / a0);
    c[4] = static_cast<float>((1.0 - alpha) / a0);
}
void applyBiquad(float* buf, int n, const float* c, float& s1, float& s2) noexcept {
    for (int i = 0; i < n; ++i) {
        const float in = buf[i];
        const float out = c[0] * in + s1;
        s1 = c[1] * in - c[3] * out + s2;
        s2 = c[2] * in - c[4] * out;
        buf[i] = out;
    }
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

void MultibandProcessor::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    rebuild();
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
    lpStates_.assign(static_cast<std::size_t>(bands - 1), {});
    hpStates_.assign(static_cast<std::size_t>(bands - 1), {});
    envelopes_.assign(static_cast<std::size_t>(bands), {});
    reductions_.assign(static_cast<std::size_t>(bands), 0.0f);
    prepared_ = true;
}

void MultibandProcessor::ensureScratch(int frames) {
    if (static_cast<int>(bufL_.size()) >= frames) return;
    const std::size_t n = static_cast<std::size_t>(frames);
    const int xovers = static_cast<int>(effect_.bands.size()) - 1;
    bufL_.assign(n, 0.0f); bufR_.assign(n, 0.0f);
    outL_.assign(n, 0.0f); outR_.assign(n, 0.0f);
    lpL_.assign(n * static_cast<std::size_t>(std::max(xovers, 1)), 0.0f);
    lpR_.assign(n * static_cast<std::size_t>(std::max(xovers, 1)), 0.0f);
    hpL_.assign(n * static_cast<std::size_t>(std::max(xovers, 1)), 0.0f);
    hpR_.assign(n * static_cast<std::size_t>(std::max(xovers, 1)), 0.0f);
}

void MultibandProcessor::process(float* left, float* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0 || effect_.bypassed) return;
    const int bands = static_cast<int>(effect_.bands.size());
    if (bands <= 0 || static_cast<int>(effect_.crossovers.size()) != bands - 1) return;
    if (!prepared_ || static_cast<int>(lpStates_.size()) != bands - 1) rebuild();
    if (!prepared_) return;
    ensureScratch(frames);
    const int xovers = bands - 1;
    for (int x = 0; x < xovers; ++x) {
        float* pl = lpL_.data() + static_cast<std::size_t>(x) * frames;
        float* pr = lpR_.data() + static_cast<std::size_t>(x) * frames;
        float* hl = hpL_.data() + static_cast<std::size_t>(x) * frames;
        float* hr = hpR_.data() + static_cast<std::size_t>(x) * frames;
        for (int i = 0; i < frames; ++i) { pl[i] = left[i]; pr[i] = right[i]; hl[i] = left[i]; hr[i] = right[i]; }
        float lp[5], hp[5];
        lrCoeffs(effect_.crossovers[static_cast<std::size_t>(x)], sampleRate_, true, lp);
        lrCoeffs(effect_.crossovers[static_cast<std::size_t>(x)], sampleRate_, false, hp);
        auto& lps = lpStates_[static_cast<std::size_t>(x)];
        auto& hps = hpStates_[static_cast<std::size_t>(x)];
        applyBiquad(pl, frames, lp, lps.s1L, lps.s2L);
        applyBiquad(pr, frames, lp, lps.s1R, lps.s2R);
        applyBiquad(hl, frames, hp, hps.s1L, hps.s2L);
        applyBiquad(hr, frames, hp, hps.s1R, hps.s2R);
    }
    for (int i = 0; i < frames; ++i) { outL_[static_cast<std::size_t>(i)] = 0.0f; outR_[static_cast<std::size_t>(i)] = 0.0f; }
    float totalGr = 0.0f;
    int activeCount = 0;
    for (int b = 0; b < bands; ++b) {
        const auto& band = effect_.bands[static_cast<std::size_t>(b)];
        const bool audible = band.enabled && !band.bypassed && (activeSolo_ < 0 || activeSolo_ == b);
        if (!audible) { reductions_[static_cast<std::size_t>(b)] = 0.0f; continue; }
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
        const float inputGain = dbToGain(band.inputGainDb);
        const float makeup = dbToGain(band.makeupDb);
        const float attack = smoothing(sampleRate_, band.attackMs);
        const float release = smoothing(sampleRate_, std::max(band.releaseMs, 10.0f));
        auto& env = envelopes_[static_cast<std::size_t>(b)];
        float grSum = 0.0f;
        for (int i = 0; i < frames; ++i) {
            bufL_[static_cast<std::size_t>(i)] *= inputGain;
            bufR_[static_cast<std::size_t>(i)] *= inputGain;
            const float level = std::max(std::fabs(bufL_[static_cast<std::size_t>(i)]), std::fabs(bufR_[static_cast<std::size_t>(i)]));
            env.env += (level - env.env) * (level > env.env ? attack : release);
            const float targetDb = multibandStaticGainDb(gainToDb(env.env), band);
            const float targetGain = dbToGain(targetDb) * makeup;
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
        const float rate = smoothing(sampleRate_, 200.0f);
        autoMakeupDb_ += (avgGr - autoMakeupDb_) * rate;
        const float g = dbToGain(autoMakeupDb_);
        for (int i = 0; i < frames; ++i) { left[i] *= g; right[i] *= g; }
    }
}

} // namespace pluginrack
