#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <pluginrack/ClockFailover.h>

namespace {

using pluginrack::ClockAction;
using pluginrack::ClockCandidate;

ClockCandidate candidate(std::uint32_t id, bool open, int rate) {
    ClockCandidate value;
    value.outputId = id;
    value.clockCapable = true;
    value.deviceOpen = open;
    value.sampleRate = rate;
    return value;
}

} // namespace

TEST_CASE("promotion picks the next live output in the same rate class",
          "[clock][failover]") {
    const std::vector<ClockCandidate> candidates = {
        candidate(1, true, 48000),   // the lost clock
        candidate(2, true, 44100),   // wrong rate class
        candidate(3, true, 48000),   // the correct promotion
        candidate(4, true, 48000),
    };
    const auto decision = pluginrack::chooseClockFailover(candidates, 1, 48000);
    CHECK(decision.action == ClockAction::Promote);
    CHECK(decision.outputId == 3);
    CHECK(decision.sampleRate == 48000);
}

TEST_CASE("internal fallback renders at the last live rate", "[clock][failover]") {
    const std::vector<ClockCandidate> candidates = {
        candidate(1, true, 48000),   // lost
        candidate(2, false, 44100),  // closed
        candidate(3, true, 96000),   // different rate class
    };
    const auto decision = pluginrack::chooseClockFailover(candidates, 1, 48000);
    CHECK(decision.action == ClockAction::InternalClock);
    CHECK(decision.outputId == 0);
    CHECK(decision.sampleRate == 48000);

    // No candidates at all still falls back to the last rate.
    const auto empty = pluginrack::chooseClockFailover({}, 1, 44100);
    CHECK(empty.action == ClockAction::InternalClock);
    CHECK(empty.sampleRate == 44100);
}

TEST_CASE("a closed or incapable output is never promoted", "[clock][failover]") {
    std::vector<ClockCandidate> candidates = {candidate(1, true, 48000)};
    ClockCandidate closed = candidate(2, false, 48000);
    ClockCandidate incapable = candidate(3, true, 48000);
    incapable.clockCapable = false;
    ClockCandidate zeroRate = candidate(4, true, 0);
    candidates.push_back(closed);
    candidates.push_back(incapable);
    candidates.push_back(zeroRate);

    const auto decision = pluginrack::chooseClockFailover(candidates, 1, 48000);
    CHECK(decision.action == ClockAction::InternalClock);
}
