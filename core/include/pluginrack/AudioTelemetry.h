#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pluginrack {

// Lock-free render telemetry: one producer (the clock audio callback) and one
// consumer (the UI). The audio thread must never block or allocate, and the UI
// must never read a half-written record, so each slot is published with a
// seqlock stamp rather than a mutex.
//
// The ring is a rolling window, not a lifetime total: counters only describe
// the samples still inside it. Lifetime totals live with the stream objects.
class AudioTelemetry {
public:
    static constexpr int kCapacity = 512;  // power of two
    static constexpr int kFillBuckets = 16;

    enum : std::uint16_t {
        flagRenderMiss = 1u << 0,
        flagOversized = 1u << 1,
        flagInputUnderrun = 1u << 2,
        flagOutputUnderrun = 1u << 3,
        flagPartialRender = 1u << 4,   // deadline spent; the tail was silenced
        flagNonFiniteScrub = 1u << 5,  // NaN/Inf was replaced with silence
    };

    struct Sample {
        std::uint32_t blockFrames = 0;
        std::uint32_t renderNanos = 0;
        std::int32_t inputFillFrames = 0;
        std::int32_t outputFillFrames = 0;
        std::uint16_t flags = 0;
    };

    struct Summary {
        int count = 0;
        double renderP50Ms = 0.0;
        double renderP95Ms = 0.0;
        double renderP99Ms = 0.0;
        double renderMaxMs = 0.0;
        double renderWorstOverBlockPercent = 0.0;
        std::uint64_t renderMisses = 0;
        std::uint64_t oversizedBlocks = 0;
        std::uint64_t inputUnderruns = 0;
        std::uint64_t outputUnderruns = 0;
        std::uint64_t partialRenders = 0;
        std::uint64_t nonFiniteScrubs = 0;
        std::array<std::uint32_t, kFillBuckets> outputFillHistogram{};
        int outputFillBucketLowFrames = 0;
        int outputFillBucketWidthFrames = 0;
    };

    // Audio thread only.
    void push(const Sample& sample) noexcept {
        const auto sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        auto& slot = slots_[static_cast<std::size_t>(sequence % kCapacity)];
        // Odd stamp = being written; the release store publishes it before the
        // data lands so a reader that sees the odd value backs off.
        slot.stamp.store(sequence * 2 + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.sample = sample;
        slot.stamp.store(sequence * 2, std::memory_order_release);
        publishedSequence_.store(sequence, std::memory_order_release);
    }

    // UI thread only. Copies whatever is consistent; a racing slot is skipped.
    Summary summarise(std::uint32_t blockFrames, double sampleRate) noexcept {
        Summary summary;
        if (scratchInUse_.exchange(true, std::memory_order_acquire)) return summary;

        const auto published = publishedSequence_.load(std::memory_order_acquire);
        const auto total = static_cast<int>(std::min<std::uint64_t>(published, kCapacity));
        std::int32_t minFill = 0;
        std::int32_t maxFill = 0;
        bool haveFill = false;

        for (int i = 0; i < total; ++i) {
            const auto sequence = published - static_cast<std::uint64_t>(i);
            const auto& slot = slots_[static_cast<std::size_t>(sequence % kCapacity)];
            const auto expected = sequence * 2;
            if (slot.stamp.load(std::memory_order_acquire) != expected) continue;
            const auto sample = slot.sample;
            if (slot.stamp.load(std::memory_order_acquire) != expected) continue;

            scratch_[static_cast<std::size_t>(summary.count)] =
                static_cast<float>(sample.renderNanos) / 1.0e6f;
            fillScratch_[static_cast<std::size_t>(summary.count)] = sample.outputFillFrames;
            ++summary.count;

            if (sample.flags & flagRenderMiss) ++summary.renderMisses;
            if (sample.flags & flagOversized) ++summary.oversizedBlocks;
            if (sample.flags & flagInputUnderrun) ++summary.inputUnderruns;
            if (sample.flags & flagOutputUnderrun) ++summary.outputUnderruns;
            if (sample.flags & flagPartialRender) ++summary.partialRenders;
            if (sample.flags & flagNonFiniteScrub) ++summary.nonFiniteScrubs;

            if (!haveFill) {
                minFill = maxFill = sample.outputFillFrames;
                haveFill = true;
            } else {
                minFill = std::min(minFill, sample.outputFillFrames);
                maxFill = std::max(maxFill, sample.outputFillFrames);
            }
        }

        if (summary.count > 0) {
            std::sort(scratch_.begin(), scratch_.begin() + summary.count);
            summary.renderP50Ms = percentile(scratch_.data(), summary.count, 0.50);
            summary.renderP95Ms = percentile(scratch_.data(), summary.count, 0.95);
            summary.renderP99Ms = percentile(scratch_.data(), summary.count, 0.99);
            summary.renderMaxMs = static_cast<double>(scratch_[static_cast<std::size_t>(summary.count - 1)]);
        }

        if (blockFrames > 0 && sampleRate > 0.0) {
            const double blockPeriodMs = static_cast<double>(blockFrames) / sampleRate * 1000.0;
            if (blockPeriodMs > 0.0) {
                summary.renderWorstOverBlockPercent = summary.renderP99Ms / blockPeriodMs * 100.0;
            }
        }

        if (haveFill) {
            summary.outputFillBucketLowFrames = static_cast<int>(minFill);
            const int span = static_cast<int>(maxFill) - static_cast<int>(minFill) + 1;
            summary.outputFillBucketWidthFrames = std::max(1, span / kFillBuckets);
            for (int i = 0; i < summary.count; ++i) {
                const int fill = static_cast<int>(fillScratch_[static_cast<std::size_t>(i)])
                                 - summary.outputFillBucketLowFrames;
                const int bucket = clampBucket(fill / summary.outputFillBucketWidthFrames);
                ++summary.outputFillHistogram[static_cast<std::size_t>(bucket)];
            }
        }

        scratchInUse_.store(false, std::memory_order_release);
        return summary;
    }

    // Only safe while the producer is stopped (device close, engine teardown).
    void reset() noexcept {
        publishedSequence_.store(0, std::memory_order_release);
        nextSequence_.store(0, std::memory_order_release);
    }

private:
    static int clampBucket(int value) noexcept { return std::clamp(value, 0, kFillBuckets - 1); }

    static double percentile(const float* values, int count, double fraction) noexcept {
        if (count <= 0) return 0.0;
        const double position = fraction * static_cast<double>(count - 1);
        const auto low = static_cast<int>(position);
        const auto high = std::min(count - 1, low + 1);
        const double weight = position - static_cast<double>(low);
        const double a = static_cast<double>(values[low]);
        const double b = static_cast<double>(values[high]);
        return a + (b - a) * weight;
    }

    struct Slot {
        std::atomic<std::uint64_t> stamp{0};
        Sample sample{};
    };

    std::array<Slot, kCapacity> slots_{};
    std::array<float, kCapacity> scratch_{};
    std::array<std::int32_t, kCapacity> fillScratch_{};
    std::atomic<std::uint64_t> nextSequence_{0};
    std::atomic<std::uint64_t> publishedSequence_{0};
    std::atomic<bool> scratchInUse_{false};
};

// Health classification for a single stream, derived from the telemetry the
// render/capture paths already publish. Read-only on the audio thread: the
// tracker only accumulates counters, and the controller that acts on the
// result lives on the message thread.
struct StreamWindow {
    bool error = false;
    double underrunsPerMinute = 0.0;
    double fillPinnedToEdgeMs = 0.0;
};

enum class StreamHealth { Green, Yellow, Red };

// Tunable boundaries for classifyHealth(). The defaults are the proposed
// values; a scene may carry its own copy so a session comes back with the
// sensitivity it was left with.
struct StreamHealthThresholds {
    double yellowUnderrunsPerMinute = 1.0;
    double redUnderrunsPerMinute = 10.0;
    double pinnedToEdgeMs = 5000.0;
};

inline StreamHealth classifyHealth(
    const StreamWindow& window,
    const StreamHealthThresholds& thresholds = {}) noexcept {
    if (window.error) return StreamHealth::Red;
    if (window.underrunsPerMinute >= thresholds.redUnderrunsPerMinute) {
        return StreamHealth::Red;
    }
    if (window.underrunsPerMinute >= thresholds.yellowUnderrunsPerMinute) {
        return StreamHealth::Yellow;
    }
    if (window.fillPinnedToEdgeMs > thresholds.pinnedToEdgeMs) {
        return StreamHealth::Yellow;
    }
    return StreamHealth::Green;
}

// Bounded rolling window for one stream. Underrun timestamps are capped so a
// pathological stream cannot grow the tracker, and events older than the
// window are pruned before every read. The pinned-to-edge duration is
// accumulated while the flag is set and capped to the window length.
class StreamHealthTracker {
public:
    static constexpr int kMaxUnderrunEvents = 128;

    void configure(double windowMs = 60000.0,
                   const StreamHealthThresholds& thresholds = {}) noexcept {
        windowMs_ = windowMs > 1000.0 ? windowMs : 1000.0;
        thresholds_ = thresholds;
        reset();
    }

    StreamHealthThresholds thresholds() const noexcept { return thresholds_; }

    void noteError() noexcept { error_ = true; }

    // Called once per block from the audio thread. Never allocates.
    void onBlock(double nowMs, bool underrun, bool fillPinnedToEdge) noexcept {
        if (windowStartMs_ < 0.0) windowStartMs_ = nowMs;
        if (nowMs - windowStartMs_ > windowMs_) {
            // Roll the window; only the new block survives the reset.
            windowStartMs_ = nowMs;
            underrunCount_ = 0;
            pinnedMs_ = 0.0;
        }
        if (lastBlockMs_ >= 0.0 && lastPinned_ && nowMs > lastBlockMs_) {
            pinnedMs_ += nowMs - lastBlockMs_;
        }
        if (underrun && underrunCount_ < kMaxUnderrunEvents) {
            underrunTimes_[static_cast<std::size_t>(underrunCount_)] = nowMs;
            ++underrunCount_;
        }
        lastBlockMs_ = nowMs;
        lastPinned_ = fillPinnedToEdge;
    }

    StreamWindow window(double nowMs) const noexcept {
        StreamWindow result;
        result.error = error_;
        if (windowStartMs_ < 0.0) return result;
        prune(nowMs);
        const double minutes = windowMs_ / 60000.0;
        result.underrunsPerMinute =
            minutes > 0.0 ? static_cast<double>(underrunCount_) / minutes : 0.0;
        result.fillPinnedToEdgeMs = pinnedMs_ > windowMs_ ? windowMs_ : pinnedMs_;
        return result;
    }

    StreamHealth health(double nowMs) const noexcept {
        return classifyHealth(window(nowMs), thresholds_);
    }

    int underrunsInWindow() const noexcept { return underrunCount_; }

    void reset() noexcept {
        error_ = false;
        underrunCount_ = 0;
        pinnedMs_ = 0.0;
        lastBlockMs_ = -1.0;
        lastPinned_ = false;
        windowStartMs_ = -1.0;
    }

private:
    // Reads prune in place so the rolling window never grows; the mutable
    // state is what lets a const health snapshot do that pruning.
    void prune(double nowMs) const noexcept {
        const double cutoff = nowMs - windowMs_;
        int kept = 0;
        for (int i = 0; i < underrunCount_; ++i) {
            if (underrunTimes_[static_cast<std::size_t>(i)] >= cutoff) {
                underrunTimes_[static_cast<std::size_t>(kept)] =
                    underrunTimes_[static_cast<std::size_t>(i)];
                ++kept;
            }
        }
        underrunCount_ = kept;
    }

    double windowMs_ = 60000.0;
    StreamHealthThresholds thresholds_{};
    mutable double windowStartMs_ = -1.0;
    mutable std::array<double, kMaxUnderrunEvents> underrunTimes_{};
    mutable int underrunCount_ = 0;
    bool error_ = false;
    mutable double pinnedMs_ = 0.0;
    mutable double lastBlockMs_ = -1.0;
    mutable bool lastPinned_ = false;
};

} // namespace pluginrack
