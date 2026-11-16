#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// One spectrum window size for the whole app. The engine's analyzer tap keeps
// exactly this many frames, and every surface that draws a spectrum (EQ,
// Multiband, DeEsser, insert thumbnails) pulls a full window from it, so the
// size is defined once and derived everywhere else.
namespace analyzer {

inline constexpr int kFftSize = 8192;
inline constexpr int kFftOrder = 13; // 2^13 == kFftSize

// Display range shared by every spectrum graph. A grid line means the same
// level in the EQ, the multiband editor and the insert thumbnails, and the
// axis labels are the same list of frequencies.
inline constexpr float kMinDb = -60.0f;
inline constexpr float kMaxDb = 12.0f;
inline constexpr float kDbRange = kMaxDb - kMinDb;
inline constexpr int kDbGridStep = 12;

inline constexpr double kMinFrequencyHz = 20.0;
inline constexpr double kMaxFrequencyHz = 20000.0;

inline constexpr std::array<int, 10> kLabelFrequenciesHz{
    20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};

// Position of a value on the shared axes: 0 at the start of the range, 1 at
// the end. Every spectrum graph maps from these two functions instead of
// re-deriving the logarithmic span.
inline float frequencyToFraction(double hz) {
    const double clamped = std::clamp(hz, kMinFrequencyHz, kMaxFrequencyHz);
    return static_cast<float>(std::log(clamped / kMinFrequencyHz)
                              / std::log(kMaxFrequencyHz / kMinFrequencyHz));
}

inline double fractionToFrequency(float t) {
    return kMinFrequencyHz
        * std::pow(kMaxFrequencyHz / kMinFrequencyHz, static_cast<double>(t));
}

// 0 at kMaxDb (top of the graph), 1 at kMinDb (bottom).
inline float dbToFraction(float db) {
    return std::clamp((db - kMinDb) / kDbRange, 0.0f, 1.0f);
}

} // namespace analyzer
