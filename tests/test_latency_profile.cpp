#include <catch2/catch_test_macros.hpp>

#include "pluginrack/LatencyProfile.h"

namespace {

using Action = pluginrack::LatencyProfile::Action;
using pluginrack::LatencyProfile;

LatencyProfile::Config fastConfig() {
    LatencyProfile::Config config;
    config.dwellMs = 100;
    return config;
}

}  // namespace

TEST_CASE("LatencyProfile picks the next smaller size when the start buffer is off-list", "[latency]") {
    LatencyProfile profile;
    profile.start(480, fastConfig());

    const auto started = profile.status();
    REQUIRE(started.active);
    REQUIRE_FALSE(started.finished);
    REQUIRE_FALSE(started.failed);
    REQUIRE(started.startBuffer == 480);
    REQUIRE(started.currentBuffer == 480);
    REQUIRE(started.totalSteps == 4);  // 256, 128, 64, 32
    REQUIRE(started.stepIndex == 0);

    REQUIRE(profile.tick(0, 0.0) == Action::Apply);

    const auto applied = profile.status();
    REQUIRE(applied.currentBuffer == 256);
    REQUIRE(applied.stepIndex == 1);
    REQUIRE(profile.lastAction() == Action::Apply);
}

TEST_CASE("LatencyProfile at the smallest buffer finishes immediately", "[latency]") {
    LatencyProfile profile;
    profile.start(32, fastConfig());

    const auto status = profile.status();
    REQUIRE_FALSE(status.active);
    REQUIRE(status.finished);
    REQUIRE_FALSE(status.failed);
    REQUIRE(status.totalSteps == 0);
    REQUIRE(status.currentBuffer == 32);

    REQUIRE(profile.tick(0, 1000.0) == Action::Done);
    REQUIRE(profile.status().currentBuffer == 32);
}

TEST_CASE("LatencyProfile with no smaller sizes finishes immediately", "[latency]") {
    LatencyProfile profile;
    auto config = fastConfig();
    config.bufferSizes = {64, 128};

    profile.start(64, config);

    const auto status = profile.status();
    REQUIRE_FALSE(status.active);
    REQUIRE(status.finished);
    REQUIRE_FALSE(status.failed);
    REQUIRE(status.currentBuffer == 64);
}

TEST_CASE("LatencyProfile walks down to the smallest candidate", "[latency]") {
    LatencyProfile profile;
    profile.start(256, fastConfig());  // candidates: 128, 64, 32

    REQUIRE(profile.tick(0, 0.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 128);

    REQUIRE(profile.tick(0, 50.0) == Action::Hold);
    REQUIRE(profile.tick(0, 50.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 64);

    REQUIRE(profile.tick(0, 100.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 32);

    REQUIRE(profile.tick(0, 100.0) == Action::Done);

    const auto done = profile.status();
    REQUIRE(done.finished);
    REQUIRE_FALSE(done.failed);
    REQUIRE_FALSE(done.active);
    REQUIRE(done.currentBuffer == 32);
    REQUIRE(done.stepIndex == 3);
    REQUIRE(done.totalSteps == 3);
    REQUIRE(done.remainingMs == 0);
    REQUIRE(profile.lastAction() == Action::Done);

    // A finished run never moves again, so the app cannot drift lower.
    REQUIRE(profile.tick(0, 1000.0) == Action::Done);
    REQUIRE(profile.status().currentBuffer == 32);
}

TEST_CASE("LatencyProfile reverts to the start buffer when an event arrives", "[latency]") {
    LatencyProfile profile;
    profile.start(1024, fastConfig());

    REQUIRE(profile.tick(0, 0.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 512);

    REQUIRE(profile.tick(1, 10.0) == Action::Revert);

    const auto failed = profile.status();
    REQUIRE(failed.failed);
    REQUIRE(failed.finished);
    REQUIRE_FALSE(failed.active);
    REQUIRE(failed.currentBuffer == 1024);
    REQUIRE(profile.lastAction() == Action::Revert);

    // No further steps are taken after a revert.
    REQUIRE(profile.tick(0, 100.0) == Action::Done);
    REQUIRE(profile.status().currentBuffer == 1024);
}

TEST_CASE("LatencyProfile caps the run at maxSteps and still succeeds", "[latency]") {
    LatencyProfile profile;
    auto config = fastConfig();
    config.maxSteps = 2;

    profile.start(4096, config);  // candidates cut to 2048, 1024
    REQUIRE(profile.status().totalSteps == 2);

    REQUIRE(profile.tick(0, 0.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 2048);

    REQUIRE(profile.tick(0, 100.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 1024);

    REQUIRE(profile.tick(0, 100.0) == Action::Done);

    const auto done = profile.status();
    REQUIRE(done.finished);
    REQUIRE_FALSE(done.failed);
    REQUIRE(done.currentBuffer == 1024);
    REQUIRE(done.stepIndex == 2);
    REQUIRE(profile.lastAction() == Action::Done);
}

TEST_CASE("LatencyProfile holds until the dwell is complete", "[latency]") {
    LatencyProfile profile;
    profile.start(128, fastConfig());  // candidates: 64, 32

    REQUIRE(profile.tick(0, 0.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 64);

    REQUIRE(profile.tick(0, 99.0) == Action::Hold);
    REQUIRE(profile.status().remainingMs == 1);

    REQUIRE(profile.tick(0, 1.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 32);
    REQUIRE(profile.status().stepIndex == 2);
}

TEST_CASE("LatencyProfile reverts on the tick after a failed apply", "[latency]") {
    LatencyProfile profile;
    profile.start(128, fastConfig());

    REQUIRE(profile.tick(0, 0.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 64);

    profile.onApplied(64, true);

    const auto failed = profile.status();
    REQUIRE(failed.failed);
    REQUIRE(failed.finished);
    REQUIRE(failed.currentBuffer == 128);

    REQUIRE(profile.tick(0, 100.0) == Action::Revert);
    REQUIRE(profile.status().currentBuffer == 128);

    REQUIRE(profile.tick(0, 100.0) == Action::Done);
}

TEST_CASE("LatencyProfile restarts the dwell when the device opens a different buffer", "[latency]") {
    LatencyProfile profile;
    profile.start(128, fastConfig());

    REQUIRE(profile.tick(0, 0.0) == Action::Apply);
    REQUIRE(profile.tick(0, 40.0) == Action::Hold);

    profile.onApplied(480, false);

    const auto adopted = profile.status();
    REQUIRE(adopted.currentBuffer == 480);
    REQUIRE(adopted.remainingMs == 100);
    REQUIRE(adopted.stepIndex == 1);

    REQUIRE(profile.tick(0, 99.0) == Action::Hold);
    REQUIRE(profile.status().remainingMs == 1);

    REQUIRE(profile.tick(0, 1.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 32);
}

TEST_CASE("LatencyProfile ignores ticks while inactive", "[latency]") {
    LatencyProfile profile;

    REQUIRE(profile.tick(5, 1000.0) == Action::Done);

    const auto status = profile.status();
    REQUIRE_FALSE(status.active);
    REQUIRE_FALSE(status.finished);
    REQUIRE_FALSE(status.failed);
    REQUIRE(status.stepIndex == 0);
    REQUIRE(status.totalSteps == 0);
    REQUIRE(status.currentBuffer == 0);
    REQUIRE(profile.lastAction() == Action::Done);
}

TEST_CASE("LatencyProfile cancel resets an in-flight run", "[latency]") {
    LatencyProfile profile;
    profile.start(256, fastConfig());

    REQUIRE(profile.tick(0, 0.0) == Action::Apply);
    REQUIRE(profile.tick(0, 10.0) == Action::Hold);

    profile.cancel();

    const auto cancelled = profile.status();
    REQUIRE_FALSE(cancelled.active);
    REQUIRE_FALSE(cancelled.finished);
    REQUIRE_FALSE(cancelled.failed);
    REQUIRE(cancelled.stepIndex == 0);
    REQUIRE(cancelled.totalSteps == 0);
    REQUIRE(cancelled.startBuffer == 256);
    REQUIRE(cancelled.currentBuffer == 256);
    REQUIRE(cancelled.remainingMs == 0);

    REQUIRE(profile.tick(0, 100.0) == Action::Done);
    REQUIRE(profile.status().currentBuffer == 256);
}

TEST_CASE("LatencyProfile ignores zero and negative elapsed time", "[latency]") {
    LatencyProfile profile;
    profile.start(128, fastConfig());

    REQUIRE(profile.tick(0, 0.0) == Action::Apply);

    REQUIRE(profile.tick(0, 0.0) == Action::Hold);
    REQUIRE(profile.status().remainingMs == 100);

    REQUIRE(profile.tick(0, -500.0) == Action::Hold);
    REQUIRE(profile.status().remainingMs == 100);

    REQUIRE(profile.tick(0, 100.0) == Action::Apply);
    REQUIRE(profile.status().currentBuffer == 32);
}

TEST_CASE("LatencyProfile uses the default dwell of 45 seconds", "[latency]") {
    LatencyProfile profile;
    LatencyProfile::Config config;

    profile.start(256, config);  // candidates: 128, 64, 32

    REQUIRE(profile.tick(0, 0.0) == Action::Apply);
    REQUIRE(profile.status().remainingMs == 45000);

    REQUIRE(profile.tick(0, 1000.0) == Action::Hold);
    REQUIRE(profile.status().currentBuffer == 128);
    REQUIRE(profile.status().remainingMs == 44000);
    REQUIRE(profile.status().stepIndex == 1);
}
