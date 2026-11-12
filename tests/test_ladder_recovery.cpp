#include <catch2/catch_test_macros.hpp>

#include <pluginrack/FallbackLadder.h>

namespace {

using pluginrack::FallbackLadder;

FallbackLadder::Config recoveryConfig() {
    FallbackLadder::Config config;
    config.eventsToStep = 3;
    config.windowMs = 5000.0;
    config.cooldownMs = 1000.0;
    config.cleanMs = 10000.0;
    config.recoveryCooldownMs = 30000.0;
    return config;
}

// Three events inside the window and an apply, starting from nowMs.
FallbackLadder::Step driveOneStep(FallbackLadder& ladder, double nowMs) {
    FallbackLadder::Step step = FallbackLadder::Step::None;
    for (int i = 0; i < 3; ++i) step = ladder.onEvent(nowMs + i);
    if (step != FallbackLadder::Step::None) ladder.onApplied(step, nowMs + 2);
    return step;
}

} // namespace

TEST_CASE("recovery waits for the clean window", "[latency][fallback][recovery]") {
    FallbackLadder ladder;
    ladder.configure(recoveryConfig());

    REQUIRE(driveOneStep(ladder, 0.0) == FallbackLadder::Step::GrowFifo);
    REQUIRE(ladder.level() == 1);

    // The last event landed at 2 ms; the clean window is 10 s.
    CHECK(ladder.maybeRecover(5000.0) == FallbackLadder::Step::None);
    CHECK(ladder.level() == 1);

    CHECK(ladder.maybeRecover(10002.0) == FallbackLadder::Step::GrowFifo);
    CHECK(ladder.level() == 0);
    CHECK(ladder.maybeRecover(20000.0) == FallbackLadder::Step::None);
}

TEST_CASE("the recovery cooldown caps recovery at one rung per interval",
          "[latency][fallback][recovery]") {
    FallbackLadder ladder;
    ladder.configure(recoveryConfig());

    REQUIRE(driveOneStep(ladder, 0.0) == FallbackLadder::Step::GrowFifo);
    REQUIRE(driveOneStep(ladder, 2000.0) == FallbackLadder::Step::RaiseBuffer);
    REQUIRE(ladder.level() == 2);

    // 10 s after the last event (2002 ms) the first rung comes back.
    CHECK(ladder.maybeRecover(12002.0) == FallbackLadder::Step::RaiseBuffer);
    REQUIRE(ladder.level() == 1);

    // Inside the 30 s cooldown the second rung is held.
    CHECK(ladder.maybeRecover(12003.0) == FallbackLadder::Step::None);
    CHECK(ladder.level() == 1);

    // A full cooldown after the first recovery, the next rung follows.
    CHECK(ladder.maybeRecover(42002.0) == FallbackLadder::Step::GrowFifo);
    CHECK(ladder.level() == 0);
}

TEST_CASE("a pinned rung disables recovery entirely",
          "[latency][fallback][recovery]") {
    FallbackLadder ladder;
    ladder.configure(recoveryConfig());
    ladder.setPinned(true);

    REQUIRE(driveOneStep(ladder, 0.0) == FallbackLadder::Step::GrowFifo);
    REQUIRE(ladder.level() == 1);

    CHECK(ladder.maybeRecover(100000.0) == FallbackLadder::Step::None);
    CHECK(ladder.level() == 1);

    ladder.setPinned(false);
    CHECK(ladder.maybeRecover(100000.0) == FallbackLadder::Step::GrowFifo);
    CHECK(ladder.level() == 0);
}
