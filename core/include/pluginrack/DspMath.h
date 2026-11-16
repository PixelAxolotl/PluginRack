#pragma once

#include <algorithm>
#include <cmath>

namespace pluginrack::dsp {

// One home for the scalar maths every effect used to copy: dB conversion,
// one-pole smoothing, and the RBJ biquad set. Effects own their state; this
// header only produces coefficients and applies them.

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kButterworthQ = 0.7071067811865476;
inline constexpr double kMinQ = 0.1;
inline constexpr double kMaxQ = 20.0;

// Lowest level gainToDb reports: below this every value reads as silence.
inline constexpr float kGainFloor = 1.0e-8f;
// Coefficients are recomputed per band on the message thread only, but state
// can still decay into denormals or be poisoned by a non-finite input sample.
inline constexpr float kDenormalFloor = 1.0e-30f;

inline float dbToGain(float db) noexcept { return std::pow(10.0f, db / 20.0f); }

inline float gainToDb(float gain) noexcept {
    return 20.0f * std::log10(std::max(gain, kGainFloor));
}

// One-pole coefficient for a time constant in milliseconds. The floor keeps a
// zero or negative setting from freezing the follower.
inline float smoothingCoeff(double sampleRateHz, float ms) noexcept {
    return 1.0f - std::exp(-1.0f / (static_cast<float>(sampleRateHz) * std::max(ms, 0.1f) * 0.001f));
}

// Transposed direct form II coefficients (a0 normalised to 1).
struct Biquad {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
};

inline bool isFinite(const Biquad& c) noexcept {
    return std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) &&
           std::isfinite(c.a1) && std::isfinite(c.a2);
}

inline void flushState(float& s) noexcept {
    if (!std::isfinite(s) || (s < kDenormalFloor && s > -kDenormalFloor)) {
        s = 0.0f;
    }
}

inline float applyBiquadSample(float x, const Biquad& c, float& s1, float& s2) noexcept {
    const float y = c.b0 * x + s1;
    s1 = c.b1 * x - c.a1 * y + s2;
    s2 = c.b2 * x - c.a2 * y;
    flushState(s1);
    flushState(s2);
    return y;
}

inline void applyBiquad(float* buf, int frames, const Biquad& c, float& s1, float& s2) noexcept {
    for (int i = 0; i < frames; ++i) {
        buf[i] = applyBiquadSample(buf[i], c, s1, s2);
    }
}

// Frequency a filter may use at this rate: inside the caller's domain and
// below Nyquist. Every derivative clamps here instead of at its call site.
inline double clampFrequency(double freqHz, double sampleRateHz, double minHz = 10.0,
                             double maxHz = 20000.0) noexcept {
    const double nyquistLimit = sampleRateHz * 0.5 - 1.0;
    const double ceiling = std::min(maxHz, nyquistLimit);
    return std::clamp(freqHz, minHz, std::max(ceiling, minHz));
}

inline double clampQ(double q) noexcept { return std::clamp(q, kMinQ, kMaxQ); }

// Shared RBJ denominators: b/a normalised by a0.
inline Biquad normalise(double b0, double b1, double b2, double a0, double a1, double a2) noexcept {
    const float inv = static_cast<float>(1.0 / a0);
    Biquad c;
    c.b0 = static_cast<float>(b0) * inv;
    c.b1 = static_cast<float>(b1) * inv;
    c.b2 = static_cast<float>(b2) * inv;
    c.a1 = static_cast<float>(a1) * inv;
    c.a2 = static_cast<float>(a2) * inv;
    return c;
}

inline Biquad lowPass(double freqHz, double sampleRateHz, double q = kButterworthQ,
                      double minHz = 10.0, double maxHz = 20000.0) noexcept {
    const double f = clampFrequency(freqHz, sampleRateHz, minHz, maxHz);
    const double w = 2.0 * kPi * f / sampleRateHz;
    const double alpha = std::sin(w) / (2.0 * clampQ(q));
    const double cosW = std::cos(w);
    return normalise((1.0 - cosW) * 0.5, 1.0 - cosW, (1.0 - cosW) * 0.5,
                     1.0 + alpha, -2.0 * cosW, 1.0 - alpha);
}

inline Biquad highPass(double freqHz, double sampleRateHz, double q = kButterworthQ,
                       double minHz = 10.0, double maxHz = 20000.0) noexcept {
    const double f = clampFrequency(freqHz, sampleRateHz, minHz, maxHz);
    const double w = 2.0 * kPi * f / sampleRateHz;
    const double alpha = std::sin(w) / (2.0 * clampQ(q));
    const double cosW = std::cos(w);
    return normalise((1.0 + cosW) * 0.5, -(1.0 + cosW), (1.0 + cosW) * 0.5,
                     1.0 + alpha, -2.0 * cosW, 1.0 - alpha);
}

// Constant skirt gain, peak gain = Q. Unity 0 dB in the pass band, so a
// de-esser can subtract it from the input and leave the rest untouched.
inline Biquad bandPass(double freqHz, double sampleRateHz, double q = kButterworthQ,
                       double minHz = 10.0, double maxHz = 20000.0) noexcept {
    const double f = clampFrequency(freqHz, sampleRateHz, minHz, maxHz);
    const double w = 2.0 * kPi * f / sampleRateHz;
    const double alpha = std::sin(w) / (2.0 * clampQ(q));
    const double cosW = std::cos(w);
    return normalise(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cosW, 1.0 - alpha);
}

// RBJ bell. Centre gain is 10^(gainDb/20), which needs the a0 denominator
// 1 +/- alpha/A (using 1 +/- alpha halves the boost).
inline Biquad bell(double freqHz, double q, double gainDb, double sampleRateHz,
                   double minHz = 10.0, double maxHz = 20000.0) noexcept {
    const double f = clampFrequency(freqHz, sampleRateHz, minHz, maxHz);
    const double w = 2.0 * kPi * f / sampleRateHz;
    const double alpha = std::sin(w) / (2.0 * clampQ(q));
    const double a = std::pow(10.0, gainDb / 40.0);
    const double cosW = std::cos(w);
    return normalise(1.0 + alpha * a, -2.0 * cosW, 1.0 - alpha * a,
                     1.0 + alpha / a, -2.0 * cosW, 1.0 - alpha / a);
}

inline Biquad lowShelf(double freqHz, double q, double gainDb, double sampleRateHz,
                       double minHz = 10.0, double maxHz = 20000.0) noexcept {
    const double f = clampFrequency(freqHz, sampleRateHz, minHz, maxHz);
    const double w = 2.0 * kPi * f / sampleRateHz;
    const double cosW = std::cos(w);
    const double a = std::pow(10.0, gainDb / 40.0);
    // RBJ shelf slope S, not a bell Q: S above 1 makes the radicand negative
    // (Q 3 and +12 dB is a normal user setting), so clamp the slope and the
    // radicand. A NaN here poisons every sample the strip outputs.
    const double slope = std::min(clampQ(q), 1.0);
    const double alpha = std::sin(w) * 0.5 *
        std::sqrt(std::max(0.0, (a + 1.0 / a) * (1.0 / slope - 1.0) + 2.0));
    const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
    return normalise(a * ((a + 1.0) + (a - 1.0) * cosW + twoSqrtAAlpha),
                     -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW),
                     a * ((a + 1.0) + (a - 1.0) * cosW - twoSqrtAAlpha),
                     (a + 1.0) - (a - 1.0) * cosW + twoSqrtAAlpha,
                     2.0 * ((a - 1.0) - (a + 1.0) * cosW),
                     (a + 1.0) - (a - 1.0) * cosW - twoSqrtAAlpha);
}

inline Biquad highShelf(double freqHz, double q, double gainDb, double sampleRateHz,
                        double minHz = 10.0, double maxHz = 20000.0) noexcept {
    const double f = clampFrequency(freqHz, sampleRateHz, minHz, maxHz);
    const double w = 2.0 * kPi * f / sampleRateHz;
    const double cosW = std::cos(w);
    const double a = std::pow(10.0, gainDb / 40.0);
    // Same shelf-slope rule as lowShelf.
    const double slope = std::min(clampQ(q), 1.0);
    const double alpha = std::sin(w) * 0.5 *
        std::sqrt(std::max(0.0, (a + 1.0 / a) * (1.0 / slope - 1.0) + 2.0));
    const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
    return normalise(a * ((a + 1.0) - (a - 1.0) * cosW + twoSqrtAAlpha),
                     2.0 * a * ((a - 1.0) - (a + 1.0) * cosW),
                     a * ((a + 1.0) - (a - 1.0) * cosW - twoSqrtAAlpha),
                     (a + 1.0) + (a - 1.0) * cosW + twoSqrtAAlpha,
                     -2.0 * ((a - 1.0) + (a + 1.0) * cosW),
                     (a + 1.0) + (a - 1.0) * cosW - twoSqrtAAlpha);
}

} // namespace pluginrack::dsp
