#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace pluginrack {

// Peak meter store. Peaks are linear amplitude, post-fader and post-send.
// observe*() keeps the larger of the stored and new peak; advance() applies
// a 20 dB/s release. Ids are registered on first observe (MixerEngine does
// this in prepare()), so render() itself never allocates.
class MeterBank {
public:
    void observeChannel(std::uint32_t id, float peak) noexcept;
    void observeOutput(std::uint32_t id, float peak) noexcept;
    void advance(int numFrames, double sampleRate) noexcept; // 20 dB/s release
    float channelPeak(std::uint32_t id) const noexcept;      // 0.0f when unknown
    float outputPeak(std::uint32_t id) const noexcept;       // 0.0f when unknown
    void reset() noexcept;

private:
    using PeakMap = std::unordered_map<std::uint32_t, std::unique_ptr<std::atomic<float>>>;

    static void observe(PeakMap& peaks, std::uint32_t id, float peak) noexcept;
    static void decay(PeakMap& peaks, float multiplier) noexcept;
    static float peakOf(const PeakMap& peaks, std::uint32_t id) noexcept;

    PeakMap channelPeaks_;
    PeakMap outputPeaks_;
};

} // namespace pluginrack
