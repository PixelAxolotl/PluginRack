#include <catch2/catch_test_macros.hpp>

#include "pluginrack/DelayTargetController.h"

namespace {

using pluginrack::DelayTargetController;

DelayTargetController::Config fastConfig() {
    DelayTargetController::Config config;
    config.blockFrames = 128;
    config.shrinkStepSamples = 16;
    config.shrinkAfterMs = 100;
    config.shrinkSettleMs = 10;
    return config;
}

DelayTargetController makeController(const DelayTargetController::Config& config, int target) {
    DelayTargetController controller;
    controller.configure(config);
    controller.reset(target);
    return controller;
}

int calmBlock(DelayTargetController& controller, int fillError = 0, double elapsedMs = 1.0) {
    return controller.onBlock(controller.target(), false, false, fillError, elapsedMs);
}

} // namespace

TEST_CASE("floor derives from the block size and honours an explicit minimum") {
    DelayTargetController controller;
    DelayTargetController::Config config;

    config.blockFrames = 128;
    controller.configure(config);
    CHECK(controller.floor() == 32);
    CHECK(controller.target() == 32);

    config.blockFrames = 512;
    controller.configure(config);
    CHECK(controller.floor() == 128);

    config.minDelaySamples = 200;
    controller.configure(config);
    CHECK(controller.floor() == 200);
}

TEST_CASE("an underrun grows by max(64, block/4) and never shrinks in that call") {
    auto controller = makeController(fastConfig(), 512);

    // One millisecond short of the first shrink.
    for (int ms = 0; ms < 99; ++ms) {
        REQUIRE(calmBlock(controller) == 512);
    }
    REQUIRE(controller.target() == 512);

    // The starving block grows instead, and restarts the calm window.
    REQUIRE(controller.onBlock(512, true, false, 0, 1.0) == 576);
    REQUIRE(calmBlock(controller) == 576);
}

TEST_CASE("growth step follows the block size and the explicit override") {
    DelayTargetController controller;
    DelayTargetController::Config config;
    config.blockFrames = 512;

    controller.configure(config);
    controller.reset(1024);
    CHECK(controller.onBlock(1024, true, false, 0, 1.0) == 1152);

    config.growStepSamples = 7;
    controller.configure(config);
    controller.reset(1024);
    CHECK(controller.onBlock(1024, true, false, 0, 1.0) == 1031);
}

TEST_CASE("repeated underruns grow up to the maximum and stop there") {
    auto config = fastConfig();
    config.maxDelaySamples = 256;
    auto controller = makeController(config, 128);

    CHECK(controller.onBlock(128, true, false, 0, 1.0) == 192);
    CHECK(controller.onBlock(192, true, false, 0, 1.0) == 256);
    for (int block = 0; block < 32; ++block) {
        controller.onBlock(controller.target(), true, false, 0, 1.0);
    }
    CHECK(controller.target() == 256);
}

TEST_CASE("calm shrinks exactly one step at shrinkAfterMs and not before") {
    auto controller = makeController(fastConfig(), 512);

    for (int ms = 0; ms < 99; ++ms) {
        REQUIRE(calmBlock(controller) == 512);
    }
    CHECK(controller.target() == 512);
    CHECK(calmBlock(controller) == 496);
}

TEST_CASE("a second shrink waits for the settle time") {
    auto controller = makeController(fastConfig(), 512);

    for (int ms = 0; ms < 99; ++ms) {
        REQUIRE(calmBlock(controller) == 512);
    }
    REQUIRE(calmBlock(controller) == 496);

    for (int ms = 0; ms < 9; ++ms) {
        REQUIRE(calmBlock(controller) == 496);
    }
    REQUIRE(calmBlock(controller) == 480);
}

TEST_CASE("a large fill error blocks shrinking and a small one allows it") {
    auto blocked = makeController(fastConfig(), 512);
    for (int ms = 0; ms < 500; ++ms) {
        blocked.onBlock(blocked.target(), false, false, 128, 1.0);
    }
    CHECK(blocked.target() == 512);

    auto allowed = makeController(fastConfig(), 512);
    for (int ms = 0; ms < 99; ++ms) {
        allowed.onBlock(allowed.target(), false, false, 16, 1.0);
    }
    CHECK(allowed.target() == 512);
    CHECK(allowed.onBlock(allowed.target(), false, false, 16, 1.0) == 496);
}

TEST_CASE("shrinking stops at the floor") {
    auto controller = makeController(fastConfig(), 64);
    int target = 64;
    for (int ms = 0; ms < 5000; ++ms) {
        target = calmBlock(controller);
    }
    CHECK(target == controller.floor());
    CHECK(target == 32);
}

TEST_CASE("an overrun resets the calm window without growing") {
    auto controller = makeController(fastConfig(), 512);
    for (int ms = 0; ms < 99; ++ms) {
        REQUIRE(calmBlock(controller) == 512);
    }

    REQUIRE(controller.onBlock(512, false, true, 0, 1.0) == 512);

    for (int ms = 0; ms < 99; ++ms) {
        REQUIRE(calmBlock(controller) == 512);
    }
    REQUIRE(calmBlock(controller) == 496);
}

TEST_CASE("reset clamps into range and clears the timers") {
    auto config = fastConfig();
    config.maxDelaySamples = 400;
    auto controller = makeController(config, 512);
    CHECK(controller.target() == 400);

    for (int ms = 0; ms < 50; ++ms) {
        calmBlock(controller);
    }
    controller.reset(10);
    CHECK(controller.target() == controller.floor());

    auto other = makeController(config, 400);
    for (int ms = 0; ms < 50; ++ms) {
        calmBlock(other);
    }
    other.reset(400);
    for (int ms = 0; ms < 99; ++ms) {
        REQUIRE(calmBlock(other) == 400);
    }
    REQUIRE(calmBlock(other) == 384);
}

TEST_CASE("targets are clamped into range on every call") {
    auto config = fastConfig();
    config.maxDelaySamples = 400;
    auto controller = makeController(config, 400);

    CHECK(controller.onBlock(9999, false, false, 0, 1.0) == 400);
    CHECK(controller.onBlock(-5000, false, false, 0, 1.0) == controller.floor());
}

TEST_CASE("production defaults never shrink before twenty seconds of calm") {
    DelayTargetController controller;
    controller.configure(DelayTargetController::Config{});
    controller.reset(4096);

    for (int block = 0; block < 79; ++block) {
        REQUIRE(calmBlock(controller, 0, 250.0) == 4096);
    }
    REQUIRE(controller.target() == 4096);
    REQUIRE(calmBlock(controller, 0, 250.0) == 4080);

    for (int block = 0; block < 7; ++block) {
        REQUIRE(calmBlock(controller, 0, 250.0) == 4080);
    }
    REQUIRE(calmBlock(controller, 0, 250.0) == 4064);
}

TEST_CASE("a starved target is never given back below the point that starved") {
    // Shrink-grow-shrink would otherwise ratchet the delay up forever: give 16
    // back, starve, grow 64, settle, give 16 back...
    auto controller = makeController(fastConfig(), 160);
    REQUIRE(controller.target() == 160);

    const int grown = controller.onBlock(160, true, false, 0, 0.0);
    CHECK(grown == 224);
    CHECK(controller.floor() == 224);

    int target = grown;
    for (int i = 0; i < 200; ++i) {
        target = controller.onBlock(target, false, false, 0, 50.0);
    }
    CHECK(target == 224);
}

TEST_CASE("reset forgets the learned floor") {
    auto controller = makeController(fastConfig(), 160);
    (void) controller.onBlock(160, true, false, 0, 0.0);
    CHECK(controller.floor() == 224);
    controller.reset(160);
    CHECK(controller.floor() == 32);
    CHECK(controller.target() == 160);
}

TEST_CASE("the controller is deterministic for identical event sequences") {
    auto run = []() {
        auto controller = makeController(fastConfig(), 256);
        int last = 0;
        for (int ms = 0; ms < 2000; ++ms) {
            const bool underrun = (ms % 137) == 0;
            const int fillError = (ms % 11) == 0 ? 128 : 4;
            last = controller.onBlock(controller.target(), underrun, false, fillError, 1.0);
        }
        return last;
    };
    CHECK(run() == run());
}
