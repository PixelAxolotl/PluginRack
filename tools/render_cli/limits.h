#pragma once

// Budgets for pluginrack-render (PR-03). A huge scene or render length must
// be rejected before it exhausts memory, CPU, or disk. The scene cap matches
// the GUI loader in SceneBridge.cpp; the render caps bound what a single
// invocation may allocate.

#include <cstdint>

namespace render_limits {

// Same as the GUI scene loader limit (8 MiB).
constexpr std::int64_t kMaxSceneFileBytes = 8 * 1024 * 1024;
// One hour of audio is already generous for a sine test render.
constexpr double kMaxSeconds = 3600.0;
// Total float samples across all outputs: 2^30 floats is ~4 GiB, still large.
constexpr std::int64_t kMaxOutputSamples = 1073741824;

inline bool sceneWithinLimit(std::int64_t bytes) {
    return bytes >= 0 && bytes <= kMaxSceneFileBytes;
}

inline bool secondsWithinLimit(double seconds, double sampleRate) {
    return seconds > 0.0 && seconds <= kMaxSeconds && sampleRate > 0.0
        && seconds * sampleRate <= 2147483647.0;
}

// frames * 2 channels * outputs, checked without overflowing.
inline bool outputWithinLimit(double frames, std::int64_t outputs) {
    if (frames < 0.0 || outputs < 0) {
        return false;
    }
    const double total = frames * 2.0 * static_cast<double>(outputs);
    return total <= static_cast<double>(kMaxOutputSamples);
}

} // namespace render_limits
