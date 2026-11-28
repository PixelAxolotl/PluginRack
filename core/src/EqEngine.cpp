#include "pluginrack/EqEngine.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {

namespace {

constexpr float kDelaySnapSamples = 0.01f;
// Delay glide: one-pole with a 20 ms time constant, snapped to the target
// once within a hundredth of a sample.
constexpr double kDelaySmoothSeconds = 0.02;

bool paramsFinite(const EqBand& band) noexcept {
    return std::isfinite(band.freqHz) && std::isfinite(band.gainDb) && std::isfinite(band.q);
}

} // namespace

int eqBandStageCount(const EqBand& band) noexcept {
    if (band.type != EqType::HighPass && band.type != EqType::LowPass) {
        return 1; // slope is a cut-off concept; bells and shelves stay single
    }
    switch (band.slopeDbPerOct) {
    case 24: return 2;
    case 48: return 4;
    default: return 1; // 12 dB/oct, and whatever an unvalidated scene holds
    }
}

std::vector<EqBand> defaultEqBands() {
    return {
        EqBand{EqType::HighPass, 80.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::LowShelf, 200.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::Peak, 500.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::Peak, 1000.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::Peak, 3000.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::HighShelf, 8000.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::LowPass, 12000.0f, 0.0f, 0.707f, false, false},
    };
}

int eqLatencySamples(bool eqEnabled, bool eqBypassed, float eqDelayMs,
                     double sampleRate) noexcept {
    if (!eqEnabled || eqBypassed || !(eqDelayMs > 0.0f) || !std::isfinite(eqDelayMs)
        || !(sampleRate > 0.0)) {
        return 0;
    }
    // Same clamp the engine applies: setDelayMs bounds the requested delay to
    // [0, kMaxDelayMs], and ensureDelayLine sizes the ring for exactly that.
    const float bounded = std::min(eqDelayMs, EqEngine::kMaxDelayMs);
    return static_cast<int>(std::lround(static_cast<double>(bounded) * 0.001 * sampleRate));
}

void EqEngine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    // Restart the filter history along with the delay line: every other
    // built-in processor flushes its states on prepare(), and stale biquad
    // memory from the previous rate leaks into the first block after a
    // device or rate change.
    for (auto& filter : filters_) {
        clearState(filter);
    }
    delaySmooth_ = sampleRate_ > 0.0
                       ? static_cast<float>(1.0 - std::exp(-1.0 / (kDelaySmoothSeconds * sampleRate_)))
                       : 0.0f;
    // A rate/buffer change restarts the chain: drop the delay history and let
    // setDelayMs() re-derive the length in samples for the new rate.
    delayL_.clear();
    delayR_.clear();
    delayCapacity_ = 0;
    delayWrite_ = 0;
    delaySamples_ = 0.0f;
    delayCurrentSamples_ = 0.0f;
    rebuild();
    if (delayMs_ > 0.0f) {
        setDelayMs(delayMs_);
    }
}

void EqEngine::setDelayMs(float delayMs) {
    delayMs_ = std::isfinite(delayMs) ? std::clamp(delayMs, 0.0f, kMaxDelayMs) : 0.0f;
    if (sampleRate_ <= 0.0) {
        return; // prepare() sizes the line once the rate is known
    }
    delaySamples_ = static_cast<float>(static_cast<double>(delayMs_) * 0.001 * sampleRate_);
    if (delaySamples_ > 0.0f) {
        ensureDelayLine(); // 0 ms never allocates; process() stays bit-exact
    }
}

void EqEngine::ensureDelayLine() {
    // One kMaxDelayMs window plus the interpolation tap: the smoothed read
    // distance can never outrun the buffer, whatever the knob is dragged to.
    const int needed = static_cast<int>(static_cast<double>(kMaxDelayMs) * 0.001 * sampleRate_) + 2;
    if (needed <= delayCapacity_) {
        return;
    }
    delayL_.assign(static_cast<std::size_t>(needed), 0.0f);
    delayR_.assign(static_cast<std::size_t>(needed), 0.0f);
    delayCapacity_ = needed;
    delayWrite_ = 0;
    delayCurrentSamples_ = delaySamples_; // first block already at the target
}

bool EqEngine::delayActive() const noexcept {
    // Either end of the glide keeps the line running so a ramp to or from
    // 0 ms stays continuous.
    return delayCapacity_ > 0 && (delayCurrentSamples_ >= 0.5f || delaySamples_ >= 0.5f);
}

void EqEngine::applyDelay(float* L, float* R, int n) noexcept {
    const int capacity = delayCapacity_;
    float* lineL = delayL_.data();
    float* lineR = delayR_.data();
    const float target = delaySamples_;
    int write = delayWrite_;
    float current = delayCurrentSamples_;

    for (int i = 0; i < n; ++i) {
        current += (target - current) * delaySmooth_;
        if (std::fabs(target - current) < kDelaySnapSamples) {
            current = target;
        }

        lineL[write] = L[i];
        lineR[write] = R[i];

        double read = static_cast<double>(write) - static_cast<double>(current);
        if (read < 0.0) {
            read += static_cast<double>(capacity);
        }
        const int i0 = static_cast<int>(read);
        const float frac = static_cast<float>(read - static_cast<double>(i0));
        const int i1 = (i0 + 1 == capacity) ? 0 : i0 + 1;

        L[i] = lineL[i0] + frac * (lineL[i1] - lineL[i0]);
        R[i] = lineR[i0] + frac * (lineR[i1] - lineR[i0]);

        if (++write == capacity) {
            write = 0;
        }
    }

    delayWrite_ = write;
    delayCurrentSamples_ = current;
}

void EqEngine::setBands(const std::vector<EqBand>& bands) {
    bands_ = bands;
    if (bands_.size() > kNumBands) {
        bands_.resize(kNumBands); // fixed seven bands; extras ignored
    }
    rebuild();
}

void EqEngine::rebuild() {
    activeMask_ = 0;
    if (sampleRate_ <= 0.0) {
        return;
    }

    bool anySolo = false;
    for (const EqBand& band : bands_) {
        if (band.solo) {
            anySolo = true;
            break;
        }
    }

    for (int i = 0; i < kNumBands && i < static_cast<int>(bands_.size()); ++i) {
        const EqBand& band = bands_[static_cast<std::size_t>(i)];
        const bool runs = band.enabled && paramsFinite(band) && (!anySolo || band.solo);
        if (!runs) {
            continue; // band skipped entirely: process() never touches it
        }
        computeCoeffs(band, sampleRate_, filters_[i]);
        // A finite setting can still compute a non-finite coefficient (see
        // the shelf radicand). Running that band would emit NaN forever, and
        // the input-side scrub never sees it.
        if (!dsp::isFinite(filters_[i].coeffs)) {
            continue;
        }
        activeMask_ |= 1 << i;
    }
}

void EqEngine::computeCoeffs(const EqBand& band, double sampleRate, BandFilter& out) noexcept {
    out.stages = eqBandStageCount(band);
    switch (band.type) {
    case EqType::HighPass:
        out.coeffs = dsp::highPass(band.freqHz, sampleRate, dsp::kButterworthQ);
        break;
    case EqType::LowPass:
        out.coeffs = dsp::lowPass(band.freqHz, sampleRate, dsp::kButterworthQ);
        break;
    case EqType::LowShelf:
        out.coeffs = dsp::lowShelf(band.freqHz, band.q, band.gainDb, sampleRate);
        break;
    case EqType::Peak:
        out.coeffs = dsp::bell(band.freqHz, band.q, band.gainDb, sampleRate);
        break;
    case EqType::HighShelf:
        out.coeffs = dsp::highShelf(band.freqHz, band.q, band.gainDb, sampleRate);
        break;
    }
}

void EqEngine::clearState(BandFilter& filter) noexcept {
    for (int stage = 0; stage < kMaxStages; ++stage) {
        filter.s1L[stage] = 0.0f;
        filter.s2L[stage] = 0.0f;
        filter.s1R[stage] = 0.0f;
        filter.s2R[stage] = 0.0f;
    }
}

void EqEngine::setPhaseFlip(bool flip) noexcept { phaseFlip_ = flip; }

void EqEngine::process(float* L, float* R, int n) noexcept {
    if (n <= 0) {
        return;
    }
    // The delay runs ahead of the band chain and independently of it: a
    // flat EQ still aligns time, and a delayed EQ keeps its tail.
    if (delayActive()) {
        applyDelay(L, R, n);
    }
    for (int i = 0; i < kNumBands; ++i) {
        if ((activeMask_ & (1 << i)) == 0) {
            continue;
        }
        BandFilter& f = filters_[i];
        // Cascade: each stage is the same biquad with its own state, so the
        // 1-stage path stays bit-identical to the old single-stage filter.
        for (int stage = 0; stage < f.stages; ++stage) {
            const auto s = static_cast<std::size_t>(stage);
            dsp::applyBiquad(L, n, f.coeffs, f.s1L[s], f.s2L[s]);
            dsp::applyBiquad(R, n, f.coeffs, f.s1R[s], f.s2R[s]);
        }
    }
    // Polarity flip: invert phase after the chain, even on a flat EQ so the
    // flip stays live when no band is active.
    if (phaseFlip_) {
        for (int i = 0; i < n; ++i) {
            L[i] = -L[i];
            R[i] = -R[i];
        }
    }
}

void EqEngine::reset() noexcept {
    for (BandFilter& f : filters_) {
        clearState(f);
    }
    if (delayCapacity_ > 0) {
        std::fill(delayL_.begin(), delayL_.end(), 0.0f);
        std::fill(delayR_.begin(), delayR_.end(), 0.0f);
        delayWrite_ = 0;
        delayCurrentSamples_ = delaySamples_; // no glide back from stale state
    }
}

} // namespace pluginrack
