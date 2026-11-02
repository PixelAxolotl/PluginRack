#include "pluginrack/Meters.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {

namespace {

constexpr float kMaxMeterPeak = 8.0f; // +18 dB; the fader allows +12 dB.

float clampPeak(float peak) noexcept {
    if (!std::isfinite(peak)) {
        return 0.0f;
    }
    return std::clamp(peak, 0.0f, kMaxMeterPeak);
}

} // namespace

void MeterBank::observeChannel(std::uint32_t id, float peak) noexcept {
    observe(channelPeaks_, id, peak);
}

void MeterBank::observeOutput(std::uint32_t id, float peak) noexcept {
    observe(outputPeaks_, id, peak);
}

void MeterBank::observeChannelRealtime(std::uint32_t id, float peak) noexcept {
    observeRealtime(channelPeaks_, id, peak);
}

void MeterBank::observeOutputRealtime(std::uint32_t id, float peak) noexcept {
    observeRealtime(outputPeaks_, id, peak);
}

void MeterBank::advance(int numFrames, double sampleRate) noexcept {
    if (numFrames <= 0 || sampleRate <= 0.0) {
        return;
    }
    const float multiplier =
        static_cast<float>(std::pow(10.0, -static_cast<double>(numFrames) / sampleRate));
    decay(channelPeaks_, multiplier);
    decay(outputPeaks_, multiplier);
}

float MeterBank::channelPeak(std::uint32_t id) const noexcept {
    return peakOf(channelPeaks_, id);
}

float MeterBank::outputPeak(std::uint32_t id) const noexcept {
    return peakOf(outputPeaks_, id);
}

void MeterBank::reset() noexcept {
    channelPeaks_.clear();
    outputPeaks_.clear();
}

void MeterBank::observe(PeakMap& peaks, std::uint32_t id, float peak) noexcept {
    const float clamped = clampPeak(peak);
    const auto it = peaks.find(id);
    if (it == peaks.end()) {
        peaks.emplace(id, std::make_unique<std::atomic<float>>(clamped));
        return;
    }
    const float current = it->second->load(std::memory_order_relaxed);
    if (clamped > current) {
        it->second->store(clamped, std::memory_order_relaxed);
    }
}

void MeterBank::observeRealtime(PeakMap& peaks, std::uint32_t id, float peak) noexcept {
    const auto it = peaks.find(id);
    if (it == peaks.end()) {
        return;
    }
    const float clamped = clampPeak(peak);
    const float current = it->second->load(std::memory_order_relaxed);
    if (clamped > current) {
        it->second->store(clamped, std::memory_order_relaxed);
    }
}

void MeterBank::decay(PeakMap& peaks, float multiplier) noexcept {
    for (auto& entry : peaks) {
        const float decayed = entry.second->load(std::memory_order_relaxed) * multiplier;
        entry.second->store(decayed, std::memory_order_relaxed);
    }
}

float MeterBank::peakOf(const PeakMap& peaks, std::uint32_t id) noexcept {
    const auto it = peaks.find(id);
    if (it == peaks.end()) {
        return 0.0f;
    }
    return it->second->load(std::memory_order_relaxed);
}

} // namespace pluginrack
