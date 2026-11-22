#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pluginrack {

enum class ClockAction { None, Promote, InternalClock };

struct ClockCandidate {
    std::uint32_t outputId = 0;
    bool clockCapable = false; // the strip still names a real output device
    bool deviceOpen = false;   // that device's stream is live
    int sampleRate = 0;        // the rate the stream actually negotiated
};

struct ClockFailoverDecision {
    ClockAction action = ClockAction::None;
    std::uint32_t outputId = 0;
    int sampleRate = 0;
};

// Pick the next live output in the same sample-rate class. Same-rate only:
// promoting a different clock would re-prime every FIFO and change the rate
// under a session that is already playing. When no candidate qualifies the
// engine keeps rendering on a free-running internal clock at the last live
// rate so the remaining outputs drain instead of snapping.
inline ClockFailoverDecision chooseClockFailover(
    const std::vector<ClockCandidate>& candidates,
    std::uint32_t lostOutputId, int lastSampleRate) noexcept {
    for (const ClockCandidate& candidate : candidates) {
        if (candidate.outputId == lostOutputId) continue;
        if (!candidate.clockCapable || !candidate.deviceOpen) continue;
        if (candidate.sampleRate <= 0) continue;
        if (lastSampleRate > 0 && candidate.sampleRate != lastSampleRate) continue;
        return ClockFailoverDecision{ClockAction::Promote, candidate.outputId,
                                     candidate.sampleRate};
    }
    return ClockFailoverDecision{ClockAction::InternalClock, 0,
                                 lastSampleRate > 0 ? lastSampleRate : 0};
}

// A recovered output may take the clock role back only when it is open, its
// callback is live, and it negotiated the rate the session already runs at.
// Reclaiming at a different rate would re-prime every FIFO, which is exactly
// what chooseClockFailover refuses to do.
inline bool canReclaimClock(bool deviceOpen, bool deviceLive,
                            int deviceSampleRate, int engineSampleRate) noexcept {
    if (!deviceOpen || !deviceLive) return false;
    if (deviceSampleRate <= 0 || engineSampleRate <= 0) return false;
    return deviceSampleRate == engineSampleRate;
}

// Callback heartbeat arithmetic. A stream that has not reported yet (last == 0)
// is not stale: it may still be starting. Timestamps are monotonic and may
// wrap, so age is unsigned; an age beyond half the 32-bit range is treated as
// a clock that went backwards rather than as a stall.
inline bool callbackStalled(std::uint64_t lastCallbackMs,
                            std::uint64_t nowMs,
                            std::uint64_t graceMs) noexcept {
    if (lastCallbackMs == 0) return false;
    // The engine's millisecond clock is 32-bit, so wrap the subtraction there.
    const std::uint32_t age =
        static_cast<std::uint32_t>(nowMs) - static_cast<std::uint32_t>(lastCallbackMs);
    if (age > 0x80000000u) return false;
    return static_cast<std::uint64_t>(age) > graceMs;
}

// Retry policy for automatic clock reclaim. It remembers the endpoint being
// waited for, when it last tried, and how many times it has tried. Pure so the
// window, cap and every reset rule can be tested without a device.
struct ReclaimGate {
    std::uint32_t lastAttemptMs = 0;
    int attempts = 0;
    std::string device;

    void reset() noexcept {
        lastAttemptMs = 0;
        attempts = 0;
        device.clear();
    }

    // The endpoint is gone: the next plug-in starts a fresh episode.
    void onAbsent() noexcept {
        lastAttemptMs = 0;
        attempts = 0;
    }

    void onSuccess() noexcept { reset(); }

    // True when this call may start an attempt. A new endpoint name resets the
    // episode; within one episode the cooldown and the attempt cap apply.
    bool shouldAttempt(const std::string& name, std::uint32_t nowMs,
                       std::uint32_t retryMs, int maxAttempts) noexcept {
        if (device != name) {
            device = name;
            lastAttemptMs = 0;
            attempts = 0;
        }
        if (attempts >= maxAttempts) return false;
        if (lastAttemptMs != 0 && nowMs - lastAttemptMs < retryMs) return false;
        lastAttemptMs = nowMs;
        ++attempts;
        return true;
    }
};

} // namespace pluginrack
