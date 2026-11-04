#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pluginrack {

// Hysteretic target delay for the capture ring's fractional read cursor.
//
// The old code added one whole block to the delay on the first starvation and
// never gave it back, so a single glitch cost latency for the rest of the
// session. Growth stays immediate because dropping audio is worse than extra
// latency, but shrinking only happens after a long calm stretch and then moves
// in small steps that the cursor's rate correction can absorb without a
// block-sized skip.
class DelayTargetController {
public:
    struct Config {
        int blockFrames = 128;
        int minDelaySamples = 0;    // 0: derive from blockFrames
        int maxDelaySamples = 0;    // 0: unlimited
        int growStepSamples = 0;    // 0: max(64, blockFrames / 4)
        int shrinkStepSamples = 16;
        int shrinkAfterMs = 20000;  // calm time required before one shrink
        int shrinkSettleMs = 2000;  // calm time required between two shrinks
    };

    void configure(const Config& config) {
        config_ = config;
        // A zero or negative block size would make the derived floor useless.
        config_.blockFrames = std::max(1, config_.blockFrames);
        config_.minDelaySamples = std::max(0, config_.minDelaySamples);
        config_.maxDelaySamples = std::max(0, config_.maxDelaySamples);
        config_.growStepSamples = std::max(0, config_.growStepSamples);
        config_.shrinkStepSamples = std::max(0, config_.shrinkStepSamples);
        config_.shrinkAfterMs = std::max(0, config_.shrinkAfterMs);
        config_.shrinkSettleMs = std::max(0, config_.shrinkSettleMs);
        target_ = std::clamp(target_, floor(), ceiling());
        calmMs_ = 0.0;
        sinceShrinkMs_ = 0.0;
        learnedFloor_ = 0;
    }

    const Config& config() const noexcept { return config_; }

    int floor() const noexcept {
        const int configured = config_.minDelaySamples > 0
                                   ? config_.minDelaySamples
                                   : std::max(32, config_.blockFrames / 4);
        // A target that starved once is not a target to return to. Without
        // this, shrink-grow-shrink would ratchet the delay upward forever.
        return std::max(configured, learnedFloor_);
    }

    int target() const noexcept { return target_; }

    void reset(int targetSamples) noexcept {
        learnedFloor_ = 0;
        target_ = std::clamp(targetSamples, floor(), ceiling());
        calmMs_ = 0.0;
        sinceShrinkMs_ = 0.0;
    }

    // Called once per render block on the reader thread. `currentTarget` is
    // authoritative because the owner owns the ring clamp, so the controller
    // re-syncs to it instead of trusting its own history.
    int onBlock(int currentTarget, bool underrun, bool overrun,
                int fillErrorSamples, double elapsedMs) noexcept {
        const double elapsed = elapsedMs > 0.0 ? elapsedMs : 0.0;
        target_ = std::clamp(currentTarget, floor(), ceiling());

        if (underrun) {
            target_ = clampToRange(static_cast<long long>(target_) + growStep());
            learnedFloor_ = std::max(learnedFloor_, target_);
            calmMs_ = 0.0;
            sinceShrinkMs_ = 0.0;
            return target_;
        }

        const double tolerance = static_cast<double>(std::max(0, config_.blockFrames / 4));
        const bool calm =
            !overrun && std::abs(static_cast<double>(fillErrorSamples)) <= tolerance;
        if (!calm) {
            // An overrun means the writer lapped us: more delay is not the fix,
            // but the calm window still has to start over.
            calmMs_ = 0.0;
            sinceShrinkMs_ = 0.0;
            return target_;
        }

        calmMs_ += elapsed;
        sinceShrinkMs_ += elapsed;
        if (target_ > floor() && calmMs_ >= static_cast<double>(config_.shrinkAfterMs) &&
            sinceShrinkMs_ >= static_cast<double>(config_.shrinkSettleMs)) {
            target_ = clampToRange(static_cast<long long>(target_) - config_.shrinkStepSamples);
            sinceShrinkMs_ = 0.0;
        }
        return target_;
    }

private:
    int ceiling() const noexcept {
        const int configured = config_.maxDelaySamples > 0
                                   ? config_.maxDelaySamples
                                   : std::numeric_limits<int>::max();
        return std::max(floor(), configured);
    }

    int growStep() const noexcept {
        return config_.growStepSamples > 0 ? config_.growStepSamples
                                           : std::max(64, config_.blockFrames / 4);
    }

    int clampToRange(long long value) const noexcept {
        return static_cast<int>(std::clamp<long long>(value, floor(), ceiling()));
    }

    Config config_{};
    int target_ = 0;
    int learnedFloor_ = 0;
    double calmMs_ = 0.0;
    double sinceShrinkMs_ = 0.0;
};

} // namespace pluginrack
