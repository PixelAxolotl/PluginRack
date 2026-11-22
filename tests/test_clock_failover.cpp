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

TEST_CASE("only a live same-rate output can reclaim the clock", "[clock][failover]") {
    using pluginrack::canReclaimClock;
    CHECK(canReclaimClock(true, true, 48000, 48000));
    CHECK_FALSE(canReclaimClock(false, true, 48000, 48000));
    CHECK_FALSE(canReclaimClock(true, false, 48000, 48000));
    CHECK_FALSE(canReclaimClock(true, true, 44100, 48000));
    CHECK_FALSE(canReclaimClock(true, true, 0, 48000));
    CHECK_FALSE(canReclaimClock(true, true, 48000, 0));
}

TEST_CASE("the callback heartbeat ignores a not-yet-started stream",
          "[clock][failover]") {
    using pluginrack::callbackStalled;
    CHECK_FALSE(callbackStalled(0, 100000, 2000));      // no callback yet
    CHECK_FALSE(callbackStalled(1000, 1000, 2000));     // same millisecond
    CHECK_FALSE(callbackStalled(1000, 3000, 2000));     // exactly the grace
    CHECK(callbackStalled(1000, 3001, 2000));           // one past the grace
    CHECK_FALSE(callbackStalled(5000, 1000, 2000));     // reversed timestamps
    CHECK(callbackStalled(0xFFFFFF00u, 0x00001000u, 2000)); // 32-bit wrap
}

TEST_CASE("reclaim attempts are rate-limited, capped and reset per episode",
          "[clock][failover]") {
    pluginrack::ReclaimGate gate;

    CHECK(gate.shouldAttempt("A", 1000, 5000, 3));   // first try is immediate
    CHECK(gate.attempts == 1);
    CHECK_FALSE(gate.shouldAttempt("A", 5000, 5000, 3)); // 4 s < 5 s
    CHECK(gate.shouldAttempt("A", 6000, 5000, 3));       // exactly 5 s
    CHECK(gate.attempts == 2);
    CHECK(gate.shouldAttempt("A", 12000, 5000, 3));
    CHECK(gate.attempts == 3);
    CHECK_FALSE(gate.shouldAttempt("A", 30000, 5000, 3)); // capped

    // The endpoint disappeared: the next plug-in starts a fresh budget and
    // does not inherit the previous episode's cooldown.
    gate.onAbsent();
    CHECK(gate.shouldAttempt("A", 30001, 5000, 3));
    CHECK(gate.attempts == 1);

    // A different endpoint is a different episode too.
    CHECK(gate.shouldAttempt("B", 30002, 5000, 3));
    CHECK(gate.attempts == 1);
    CHECK(gate.device == "B");

    gate.onSuccess();
    CHECK(gate.attempts == 0);
    CHECK(gate.lastAttemptMs == 0);
    CHECK(gate.device.empty());

    // uint32 wrap must not pin the gate closed.
    pluginrack::ReclaimGate wrapped;
    CHECK(wrapped.shouldAttempt("A", 0xFFFFFF00u, 5000, 3));
    CHECK_FALSE(wrapped.shouldAttempt("A", 0x00000100u, 5000, 3)); // 512 < 5000
    CHECK(wrapped.shouldAttempt("A", 0x00001400u, 5000, 3));      // 5376 >= 5000
}
