#pragma once

#include <cmath>

namespace pluginrack {

// Quarantine non-finite samples at a plugin boundary. The drift resampler
// already zeroes a bad tap inside its kernel; this extends the same guarantee
// to every insert and the mix bus, so one NaN cannot ring through the
// downstream EQ/dynamics state and out of every routed output.
//
// Allocation-free, branch-predictable, and safe on the audio thread.
inline int scrubNonFinite(float* samples, int frames) noexcept {
    if (samples == nullptr || frames <= 0) return 0;
    int scrubbed = 0;
    for (int i = 0; i < frames; ++i) {
        if (std::isfinite(samples[i])) continue;
        samples[i] = 0.0f;
        ++scrubbed;
    }
    return scrubbed;
}

inline int scrubNonFinite(float* left, float* right, int frames) noexcept {
    int scrubbed = scrubNonFinite(left, frames);
    if (right != nullptr && right != left) scrubbed += scrubNonFinite(right, frames);
    return scrubbed;
}

inline int scrubNonFinite(float* const* channels, int numChannels, int frames) noexcept {
    if (channels == nullptr) return 0;
    int scrubbed = 0;
    for (int c = 0; c < numChannels; ++c) {
        scrubbed += scrubNonFinite(channels[c], frames);
    }
    return scrubbed;
}

} // namespace pluginrack
