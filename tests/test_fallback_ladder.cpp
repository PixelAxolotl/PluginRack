#include <catch2/catch_test_macros.hpp>

#include <pluginrack/FallbackLadder.h>

namespace {

using pluginrack::FallbackLadder;

FallbackLadder::Config fastConfig() {
    FallbackLadder::Config config;
    config.eventsToStep = 3;
    config.windowMs = 5000.0;
    config.cooldownMs = 1000.0;
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

TEST_CASE("a couple of events do not step the ladder", "[latency][fallback]") {
    FallbackLadder ladder;
    ladder.configure(fastConfig());
    CHECK(ladder.onEvent(0.0) == FallbackLadder::Step::None);
    CHECK(ladder.onEvent(10.0) == FallbackLadder::Step::None);
    CHECK(ladder.level() == 0);
}

TEST_CASE("three events inside the window produce one pending step",
          "[latency][fallback]") {
    FallbackLadder ladder;
    ladder.configure(fastConfig());
    CHECK(ladder.onEvent(0.0) == FallbackLadder::Step::None);
    CHECK(ladder.onEvent(10.0) == FallbackLadder::Step::None);
    CHECK(ladder.onEvent(20.0) == FallbackLadder::Step::GrowFifo);
    // Level only moves once the engine confirms the step.
    CHECK(ladder.level() == 0);
    CHECK(ladder.hasPendingStep());
    // A fourth event while pending changes nothing.
    CHECK(ladder.onEvent(30.0) == FallbackLadder::Step::None);
}

TEST_CASE("the cooldown blocks the next step until it expires", "[latency][fallback]") {
    FallbackLadder ladder;
    ladder.configure(fastConfig());
    CHECK(driveOneStep(ladder, 0.0) == FallbackLadder::Step::GrowFifo);
    CHECK(ladder.level() == 1);

    // Inside the cooldown: events are ignored and cannot bank a step.
    for (int i = 0; i < 6; ++i) CHECK(ladder.onEvent(100.0 + i) == FallbackLadder::Step::None);

    // After the cooldown, three fresh events step again.
    FallbackLadder::Step step = FallbackLadder::Step::None;
    for (int i = 0; i < 3; ++i) {
        const auto produced = ladder.onEvent(2000.0 + i);
        if (produced != FallbackLadder::Step::None) step = produced;
    }
    CHECK(step == FallbackLadder::Step::RaiseBuffer);
}

TEST_CASE("the ladder is terminal after four applied steps", "[latency][fallback]") {
    FallbackLadder ladder;
    ladder.configure(fastConfig());

    const FallbackLadder::Step expected[] = {
        FallbackLadder::Step::GrowFifo,
        FallbackLadder::Step::RaiseBuffer,
        FallbackLadder::Step::LeaveExclusive,
        FallbackLadder::Step::StableDefaults,
    };
    double now = 0.0;
    for (const auto want : expected) {
        const auto got = driveOneStep(ladder, now);
        CHECK(got == want);
        now += 2000.0;
    }
    CHECK(ladder.level() == 4);
    CHECK(ladder.isTerminal());
    CHECK(ladder.onEvent(now) == FallbackLadder::Step::None);
    CHECK(ladder.onEvent(now + 100000.0) == FallbackLadder::Step::None);
}

TEST_CASE("events spread beyond the window never accumulate", "[latency][fallback]") {
    FallbackLadder ladder;
    ladder.configure(fastConfig());
    CHECK(ladder.onEvent(0.0) == FallbackLadder::Step::None);
    CHECK(ladder.onEvent(4000.0) == FallbackLadder::Step::None);
    // The first event has aged out by now.
    CHECK(ladder.onEvent(8000.0) == FallbackLadder::Step::None);
    CHECK(ladder.eventsInWindow() == 2);
}

TEST_CASE("a backwards clock restarts the window instead of corrupting it",
          "[latency][fallback]") {
    FallbackLadder ladder;
    ladder.configure(fastConfig());
    CHECK(ladder.onEvent(1000.0) == FallbackLadder::Step::None);
    CHECK(ladder.onEvent(1010.0) == FallbackLadder::Step::None);
    CHECK(ladder.onEvent(5.0) == FallbackLadder::Step::None);
    CHECK(ladder.eventsInWindow() == 1);
}

TEST_CASE("a clean period clears the counter but keeps the level", "[latency][fallback]") {
    FallbackLadder ladder;
    ladder.configure(fastConfig());
    CHECK(driveOneStep(ladder, 0.0) == FallbackLadder::Step::GrowFifo);
    ladder.onEvent(2000.0);
    ladder.onEvent(2010.0);
    ladder.onClean(2020.0);
    CHECK(ladder.eventsInWindow() == 0);
    CHECK(ladder.level() == 1);
}

TEST_CASE("reset returns the ladder to the top", "[latency][fallback]") {
    FallbackLadder ladder;
    ladder.configure(fastConfig());
    CHECK(driveOneStep(ladder, 0.0) == FallbackLadder::Step::GrowFifo);
    ladder.reset();
    CHECK(ladder.level() == 0);
    CHECK_FALSE(ladder.isTerminal());
    CHECK(ladder.currentStep() == FallbackLadder::Step::None);
}

TEST_CASE("every step has a UI description", "[latency][fallback]") {
    CHECK_FALSE(FallbackLadder::describe(FallbackLadder::Step::GrowFifo).empty());
    CHECK_FALSE(FallbackLadder::describe(FallbackLadder::Step::RaiseBuffer).empty());
    CHECK_FALSE(FallbackLadder::describe(FallbackLadder::Step::LeaveExclusive).empty());
    CHECK_FALSE(FallbackLadder::describe(FallbackLadder::Step::StableDefaults).empty());
    CHECK(FallbackLadder::describe(FallbackLadder::Step::None).empty());
}
