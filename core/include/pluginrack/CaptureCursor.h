#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pluginrack {

// Fixed-delay read cursor for a single-producer broadcast capture ring.
//
// The writer publishes a monotonically increasing count of samples written.
// The reader keeps a cursor `targetDelay` samples behind that count and
// returns one window per render block. Reading the newest frames instead
// (the old approach) repeats or skips a whole block whenever the input and
// output device callbacks drift or jitter against each other, which is
// audible as a periodic click. This cursor advances at a slightly corrected
// rate (linear interpolation at the call site) so the delay converges without
// a block-sized jump.
class CaptureReadCursor {
public:
    struct Window {
        std::int64_t start = 0; // global index of the first sample to read
        int available = 0;      // consecutive samples present from `start`
        double fraction = 0.0;  // sub-sample offset at `start`, [0, 1)
        double ratio = 1.0;     // source samples consumed per output sample
        bool underrun = false;  // writer produced less than one full block
        bool overrun = false;   // writer lapped the cursor; window resynced
        bool fresh = false;     // false when replayed for another channel
    };

    void prepare(int capacitySamples, int targetDelaySamples,
                 double maxCorrection = 0.005) {
        capacity_ = std::max(0, capacitySamples);
        targetDelay_ = std::clamp(targetDelaySamples, 0, std::max(0, capacity_ - 1));
        maxCorrection_ = std::clamp(maxCorrection, 0.0, 0.05);
        reset();
    }

    void reset() {
        cursor_ = 0.0;
        servedEpoch_ = 0;
        primed_ = false;
        baselineWritten_ = 0;
        outputSinceBaseline_ = 0;
        ratePrimed_ = false;
        current_ = Window{};
    }

    // `written` is the producer's published total. `epoch` identifies one
    // render pass; repeated calls with the same epoch return the same window
    // so several channels sharing one capture receive identical samples.
    Window nextBlock(std::uint64_t written, int frames, std::uint64_t epoch) {
        if (capacity_ <= 0 || frames <= 0) return Window{};

        const int framesClamped = std::min(frames, capacity_ - 1);
        if (framesClamped <= 0) return Window{};

        if (primed_ && epoch == servedEpoch_) return current_;
        servedEpoch_ = epoch;

        const auto maxSamples = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
        const auto writtenClamped = static_cast<std::int64_t>(
            std::min<std::uint64_t>(written, maxSamples));

        // Leave room for the read look-ahead and at least two spare samples.
        const int target = std::min(targetDelay_,
                                    std::max(0, capacity_ - framesClamped - 2));

        if (!primed_ || cursor_ < 0.0 ||
            cursor_ > static_cast<double>(writtenClamped)) {
            cursor_ = static_cast<double>(
                std::max<std::int64_t>(0, writtenClamped - target));
            primed_ = true;
            baselineWritten_ = writtenClamped;
            outputSinceBaseline_ = 0;
            ratePrimed_ = true;
        }

        // Long-baseline writer rate: the number of input samples produced per
        // output sample. Averaging from the prime point makes callback phase
        // jitter cancel out instead of steering the rate on every block.
        double correction = 0.0;
        if (ratePrimed_ && outputSinceBaseline_ >= static_cast<std::uint64_t>(framesClamped)) {
            const double rate =
                static_cast<double>(writtenClamped - baselineWritten_) /
                static_cast<double>(outputSinceBaseline_);
            correction += rate - 1.0;
        }

        Window window;
        window.fresh = true;
        const double available =
            static_cast<double>(writtenClamped) - cursor_;
        // Reading output sample s uses indexes floor(cursor + s * ratio) and
        // that index + 1, so a full block needs one look-ahead sample.
        const double error = available - static_cast<double>(target);
        if (std::abs(error) >= 2.0 && target > 0) {
            correction += std::clamp(
                (error / static_cast<double>(target)) * maxCorrection_,
                -maxCorrection_, maxCorrection_);
        }
        const double ratio =
            1.0 + std::clamp(correction, -maxCorrection_, maxCorrection_);
        const double needed =
            static_cast<double>(framesClamped - 1) * ratio + 2.0;

        if (available < needed) {
            // Writer fell behind: deliver what exists, let the owner fade the
            // rest, then continue from the writer position. The delay is
            // rebuilt gradually by the rate correction below, never by a
            // block-sized repeat. Keep the clock-rate baseline: a late writer
            // does not mean its average rate changed.
            window.start = static_cast<std::int64_t>(std::floor(cursor_));
            window.fraction = cursor_ - std::floor(cursor_);
            window.available = static_cast<int>(std::clamp(
                std::floor(available), 0.0,
                static_cast<double>(framesClamped)));
            window.underrun = true;
            cursor_ = static_cast<double>(writtenClamped);
        } else if (available >
                   static_cast<double>(capacity_ - framesClamped - 2)) {
            // The writer lapped the cursor (the reader stalled longer than
            // the ring holds). Jump to the target delay instead of playing
            // samples the writer is already overwriting, and restart the
            // rate baseline because the stall's backlog is not clock drift.
            cursor_ = static_cast<double>(
                std::max<std::int64_t>(0, writtenClamped - target));
            window.start = static_cast<std::int64_t>(std::floor(cursor_));
            window.fraction = cursor_ - std::floor(cursor_);
            window.available = framesClamped;
            window.overrun = true;
            cursor_ += static_cast<double>(framesClamped);
            baselineWritten_ = writtenClamped;
            outputSinceBaseline_ = 0;
        } else {
            window.start = static_cast<std::int64_t>(std::floor(cursor_));
            window.fraction = cursor_ - std::floor(cursor_);
            window.available = framesClamped;
            window.ratio = ratio;
            cursor_ += static_cast<double>(framesClamped) * ratio;
        }

        outputSinceBaseline_ += static_cast<std::uint64_t>(framesClamped);

        // Safety net: never fall outside the ring's retained history.
        const double oldest =
            static_cast<double>(writtenClamped) - static_cast<double>(capacity_ - 2);
        if (oldest > 0.0 && cursor_ < oldest) cursor_ = oldest;

        current_ = window;
        return window;
    }

    int capacity() const { return capacity_; }
    int targetDelay() const { return targetDelay_; }
    bool isPrimed() const { return primed_; }

    // The audio owner may increase the delay after a real starvation without
    // resetting the fractional cursor. Keeping the cursor alive avoids the
    // block repeat/skip that a full reset would introduce at the exact moment
    // the stream is recovering.
    void setTargetDelay(int targetDelaySamples) noexcept {
        targetDelay_ = std::clamp(targetDelaySamples, 0, std::max(0, capacity_ - 1));
    }

private:
    int capacity_ = 0;
    int targetDelay_ = 0;
    double maxCorrection_ = 0.005;
    double cursor_ = 0.0;
    std::uint64_t baselineWritten_ = 0;
    std::uint64_t outputSinceBaseline_ = 0;
    std::uint64_t servedEpoch_ = 0;
    bool primed_ = false;
    bool ratePrimed_ = false;
    Window current_{};
};

} // namespace pluginrack
