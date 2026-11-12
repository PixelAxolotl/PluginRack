#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pluginrack {

// Steps an aggressive low-latency configuration back down when the audio path
// keeps reporting events. One rung at a time, with a cooldown, so a single
// glitch never rewrites the user's settings and a burst has a bounded result.
//
// The rungs are ordered by how little they cost the user: growing the FIFO
// first, then the device buffer, then giving up exclusive mode, and finally the
// stable shared defaults that always work. The ladder is deliberately terminal
// there — it never climbs back, because re-climbing is what makes an automatic
// controller audibly oscillate.
class FallbackLadder {
public:
    enum class Step {
        None,
        GrowFifo,
        RaiseBuffer,
        LeaveExclusive,
        StableDefaults
    };

    struct Config {
        int eventsToStep = 3;
        double windowMs = 5000.0;
        double cooldownMs = 10000.0;
        // Recovery is deliberately slower than degradation: one rung comes
        // back after a long clean window, and never more often than the
        // recovery cooldown. The anti-oscillation guarantee is kept; only the
        // "stuck forever" case is un-stuck.
        double cleanMs = 600000.0;         // 10 minutes without an event
        double recoveryCooldownMs = 1800000.0; // 30 minutes between recoveries
    };

    void configure(const Config& config) noexcept { config_ = config; }

    // A user pin turns the ladder into a one-way ratchet again. Recovery is
    // fully disabled, exactly like the historical terminal ladder.
    void setPinned(bool pinned) noexcept { pinned_ = pinned; }
    bool isPinned() const noexcept { return pinned_; }

    Step onEvent(double nowMs) noexcept {
        if (isTerminal()) return Step::None;
        if (pending_) return Step::None;

        if (!clockStarted_ || nowMs < lastEventMs_) {
            // Time went backwards (or this is the first event): the old window
            // says nothing about the new clock, so start clean.
            eventCount_ = 0;
        }
        clockStarted_ = true;
        lastEventMs_ = nowMs;

        if (hasApplied_ && nowMs - lastAppliedMs_ < config_.cooldownMs) {
            // Events during the cooldown are not evidence we can act on: a later
            // step has to be earned by a fresh burst, or one noisy second would
            // walk the whole ladder down.
            eventCount_ = 0;
            return Step::None;
        }

        if (eventCount_ < static_cast<int>(events_.size())) {
            events_[static_cast<std::size_t>(eventCount_)] = nowMs;
            ++eventCount_;
        }

        prune(nowMs);
        if (eventCount_ < config_.eventsToStep) return Step::None;

        pending_ = true;
        pendingStep_ = stepForLevel(level_ + 1);
        return pendingStep_;
    }

    void onApplied(Step step, double nowMs) noexcept {
        if (step == Step::None) return;
        pending_ = false;
        pendingStep_ = Step::None;
        level_ = std::min(kTerminalLevel, level_ + 1);
        lastAppliedMs_ = nowMs;
        hasApplied_ = true;
        eventCount_ = 0;
    }

    void onClean(double /*nowMs*/) noexcept { eventCount_ = 0; }

    // One careful step back up after a long clean window. Returns the step
    // being undone (so the caller can toast and log it), or None when the
    // ladder is at the top, pinned, still inside the clean window, or inside
    // the recovery cooldown. Exactly one rung per call, by construction.
    Step maybeRecover(double nowMs) noexcept {
        if (level_ <= 0 || pinned_ || pending_) return Step::None;
        if (!clockStarted_ || nowMs < lastEventMs_) return Step::None;
        if (nowMs - lastEventMs_ < config_.cleanMs) return Step::None;
        if (hasRecovered_ && nowMs - lastRecoveryMs_ < config_.recoveryCooldownMs) {
            return Step::None;
        }

        const int recoveredFrom = level_;
        --level_;
        lastRecoveryMs_ = nowMs;
        hasRecovered_ = true;
        eventCount_ = 0;
        pending_ = false;
        pendingStep_ = Step::None;
        return stepForLevel(recoveredFrom);
    }

    void reset() noexcept {
        level_ = 0;
        eventCount_ = 0;
        pending_ = false;
        pendingStep_ = Step::None;
        hasApplied_ = false;
        lastAppliedMs_ = 0.0;
        lastEventMs_ = 0.0;
        clockStarted_ = false;
        lastRecoveryMs_ = 0.0;
        hasRecovered_ = false;
        pinned_ = false;
    }

    int level() const noexcept { return level_; }
    bool isTerminal() const noexcept { return level_ >= kTerminalLevel; }
    int eventsInWindow() const noexcept { return eventCount_; }
    Step currentStep() const noexcept { return stepForLevel(level_); }
    bool hasPendingStep() const noexcept { return pending_; }

    static std::string_view describe(Step step) noexcept {
        switch (step) {
            case Step::GrowFifo: return "grew the FIFO target";
            case Step::RaiseBuffer: return "raised the buffer size";
            case Step::LeaveExclusive: return "left exclusive mode";
            case Step::StableDefaults: return "returned to stable defaults";
            case Step::None: break;
        }
        return {};
    }

private:
    static constexpr int kTerminalLevel = 4;

    static Step stepForLevel(int level) noexcept {
        switch (level) {
            case 1: return Step::GrowFifo;
            case 2: return Step::RaiseBuffer;
            case 3: return Step::LeaveExclusive;
            case 4: return Step::StableDefaults;
            default: break;
        }
        return Step::None;
    }

    void prune(double nowMs) noexcept {
        int kept = 0;
        const auto cutoff = nowMs - config_.windowMs;
        for (int i = 0; i < eventCount_; ++i) {
            if (events_[static_cast<std::size_t>(i)] >= cutoff) {
                events_[static_cast<std::size_t>(kept)] = events_[static_cast<std::size_t>(i)];
                ++kept;
            }
        }
        eventCount_ = kept;
    }

    Config config_{};
    std::array<double, 32> events_{};
    int eventCount_ = 0;
    int level_ = 0;
    bool pending_ = false;
    Step pendingStep_ = Step::None;
    bool hasApplied_ = false;
    double lastAppliedMs_ = 0.0;
    double lastEventMs_ = 0.0;
    bool clockStarted_ = false;
    double lastRecoveryMs_ = 0.0;
    bool hasRecovered_ = false;
    bool pinned_ = false;
};

} // namespace pluginrack
