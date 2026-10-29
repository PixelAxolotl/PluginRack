#include "pluginrack/EqEngine.h"

#include <cmath>

namespace pluginrack {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinFreqHz = 10.0;      // HP range starts at 10 Hz; clamps protect the rest.
constexpr double kMaxFreqHz = 20000.0;
constexpr double kMinQ = 0.1;
constexpr double kMaxQ = 10.0;
constexpr double kButterworthQ = 0.7071067811865476; // HP ignores band.q (brief: "HP Q unused")
constexpr float kDenormalFloor = 1e-30f;

bool paramsFinite(const EqBand& band) noexcept {
    return std::isfinite(band.freqHz) && std::isfinite(band.gainDb) && std::isfinite(band.q);
}

} // namespace

std::vector<EqBand> defaultSixBands() {
    return {
        EqBand{EqType::HighPass, 80.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::LowShelf, 200.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::Peak, 500.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::Peak, 1000.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::Peak, 3000.0f, 0.0f, 0.707f, true, false},
        EqBand{EqType::HighShelf, 8000.0f, 0.0f, 0.707f, true, false},
    };
}

void EqEngine::prepare(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlock;
    rebuild();
}

void EqEngine::setBands(const std::vector<EqBand>& bands) {
    bands_ = bands;
    if (bands_.size() > kNumBands) {
        bands_.resize(kNumBands); // fixed six bands; extras ignored
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
        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cosW0;
        b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
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

void EqEngine::process(float* L, float* R, int n) noexcept {
    if (n <= 0 || activeMask_ == 0) {
        return;
    }
    for (int i = 0; i < kNumBands; ++i) {
        if ((activeMask_ & (1 << i)) == 0) {
            continue;
        }
        Biquad& f = filters_[i];
        applyChannel(L, n, f, f.s1L, f.s2L);
        applyChannel(R, n, f, f.s1R, f.s2R);
    }
}

void EqEngine::reset() noexcept {
    for (Biquad& f : filters_) {
        f.s1L = 0.0f;
        f.s2L = 0.0f;
        f.s1R = 0.0f;
        f.s2R = 0.0f;
    }
}

} // namespace pluginrack
