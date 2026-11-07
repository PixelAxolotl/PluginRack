#include "pluginrack/EqEngine.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinFreqHz = 10.0;      // HP range starts at 10 Hz; clamps protect the rest.
constexpr double kMaxFreqHz = 20000.0;
constexpr double kMinQ = 0.1;
constexpr double kMaxQ = 20.0; // matches SceneValidator's eq q ceiling [0.1, 20]
constexpr double kButterworthQ = 0.7071067811865476; // HP ignores band.q (brief: "HP Q unused")
constexpr float kDenormalFloor = 1e-30f;
// Delay glide: one-pole with a 20 ms time constant, snapped to the target
// once within a hundredth of a sample.
constexpr double kDelaySmoothSeconds = 0.02;
constexpr float kDelaySnapSamples = 0.01f;

bool paramsFinite(const EqBand& band) noexcept {
    return std::isfinite(band.freqHz) && std::isfinite(band.gainDb) && std::isfinite(band.q);
}

} // namespace

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

void EqEngine::prepare(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlock;
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
        activeMask_ |= 1 << i;
    }
}

void EqEngine::computeCoeffs(const EqBand& band, double sampleRate, Biquad& out) noexcept {
    const double nyquist = sampleRate * 0.5;
    const double maxFreq = (kMaxFreqHz < nyquist) ? kMaxFreqHz : nyquist - 1.0;
    const double freq = std::fmin(std::fmax(static_cast<double>(band.freqHz), kMinFreqHz), maxFreq);
    const double q = std::fmin(std::fmax(static_cast<double>(band.q), kMinQ), kMaxQ);
    const double gainDb = static_cast<double>(band.gainDb);

    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * freq / sampleRate;
    const double sinW0 = std::sin(w0);
    const double cosW0 = std::cos(w0);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (band.type) {
    case EqType::HighPass: {
        const double alpha = sinW0 / (2.0 * kButterworthQ);
        b0 = (1.0 + cosW0) * 0.5;
        b1 = -(1.0 + cosW0);
        b2 = (1.0 + cosW0) * 0.5;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
        break;
    }
    case EqType::LowPass: {
        const double alpha = sinW0 / (2.0 * kButterworthQ);
        b0 = (1.0 - cosW0) * 0.5;
        b1 = 1.0 - cosW0;
        b2 = (1.0 - cosW0) * 0.5;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
        break;
    }
    case EqType::LowShelf: {
        const double alpha = sinW0 * 0.5 * std::sqrt((a + 1.0 / a) * (1.0 / (q * q) - 1.0) + 2.0);
        const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
        b0 = a * ((a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha);
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW0);
        b2 = a * ((a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha);
        a0 = (a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha;
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosW0);
        a2 = (a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha;
        break;
    }
    case EqType::Peak: {
        const double alpha = sinW0 / (2.0 * q);
        // RBJ cookbook bell: centre gain = a^2 = 10^(gainDb/20), which
        // requires the denominator 1 +/- alpha/a (1 +/- alpha halves it).
        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cosW0;
        b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha / a;
        break;
    }
    case EqType::HighShelf: {
        const double alpha = sinW0 * 0.5 * std::sqrt((a + 1.0 / a) * (1.0 / (q * q) - 1.0) + 2.0);
        const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
        b0 = a * ((a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha);
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosW0);
        b2 = a * ((a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha);
        a0 = (a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha;
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosW0);
        a2 = (a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha;
        break;
    }
    }

    const float inv = static_cast<float>(1.0 / a0);
    out.b0 = static_cast<float>(b0) * inv;
    out.b1 = static_cast<float>(b1) * inv;
    out.b2 = static_cast<float>(b2) * inv;
    out.a1 = static_cast<float>(a1) * inv;
    out.a2 = static_cast<float>(a2) * inv;
}

void EqEngine::applyChannel(float* buf, int n, Biquad& f, float& s1, float& s2) noexcept {
    // DF-II transposed: y[n] = b0*x[n] + s1; s1 = b1*x[n] - a1*y[n] + s2;
    // s2 = b2*x[n] - a2*y[n]. Guards flush denormals and reset non-finite state.
    for (int i = 0; i < n; ++i) {
        const float x = buf[i];
        const float y = f.b0 * x + s1;
        s1 = f.b1 * x - f.a1 * y + s2;
        s2 = f.b2 * x - f.a2 * y;
        if (!std::isfinite(s1) || (s1 < kDenormalFloor && s1 > -kDenormalFloor)) {
            s1 = 0.0f;
        }
        if (!std::isfinite(s2) || (s2 < kDenormalFloor && s2 > -kDenormalFloor)) {
            s2 = 0.0f;
        }
        buf[i] = y;
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
    if (activeMask_ != 0) {
        for (int i = 0; i < kNumBands; ++i) {
            if ((activeMask_ & (1 << i)) == 0) {
                continue;
            }
            Biquad& f = filters_[i];
            applyChannel(L, n, f, f.s1L, f.s2L);
            applyChannel(R, n, f, f.s1R, f.s2R);
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
    for (Biquad& f : filters_) {
        f.s1L = 0.0f;
        f.s2L = 0.0f;
        f.s1R = 0.0f;
        f.s2R = 0.0f;
    }
    if (delayCapacity_ > 0) {
        std::fill(delayL_.begin(), delayL_.end(), 0.0f);
        std::fill(delayR_.begin(), delayR_.end(), 0.0f);
        delayWrite_ = 0;
        delayCurrentSamples_ = delaySamples_; // no glide back from stale state
    }
}

} // namespace pluginrack
