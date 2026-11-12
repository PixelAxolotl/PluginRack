#pragma once

#include <cstdint>
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

} // namespace pluginrack
