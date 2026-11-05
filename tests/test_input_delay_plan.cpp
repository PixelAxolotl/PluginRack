#include <catch2/catch_test_macros.hpp>

#include <pluginrack/InputDelayPlan.h>

namespace {

pluginrack::InputDelayPlan::Config lowLatency() {
    pluginrack::InputDelayPlan::Config config;
    config.readDelayBlocks = 1;
    return config;
}

} // namespace

TEST_CASE("the read delay is the same whatever order the devices opened in",
          "[latency][input]") {
    // The bug: a capture that opens before the clock has negotiated used a
    // placeholder render block, so the same setup produced a shorter delay when
    // the clock was ready first and a longer one otherwise.
    pluginrack::InputDelayPlan clockFirst;
    clockFirst.open(480, 960, lowLatency());
    const int afterClockFirst = clockFirst.applyRenderBlock(960);

    pluginrack::InputDelayPlan captureFirst;
    captureFirst.open(480, 256, lowLatency());   // clock not negotiated yet
    const int afterCaptureFirst = captureFirst.applyRenderBlock(960);

    CHECK(afterClockFirst == afterCaptureFirst);
}

TEST_CASE("one block plus headroom is the minimum every load lands on",
          "[latency][input]") {
    pluginrack::InputDelayPlan plan;
    plan.open(960, 256, lowLatency());
    CHECK(plan.applyRenderBlock(960) == 960 + 32);
    // Never below what CaptureReadCursor can actually serve.
    CHECK(plan.delay() >= plan.blockFrames() + 1);
}

TEST_CASE("the conservative start holds two blocks", "[latency][input]") {
    pluginrack::InputDelayPlan::Config twoBlocks;
    pluginrack::InputDelayPlan plan;
    plan.open(480, 960, twoBlocks);
    CHECK(plan.applyRenderBlock(960) == 1920);
}

TEST_CASE("a smaller capture device never drags the delay below the render block",
          "[latency][input]") {
    pluginrack::InputDelayPlan plan;
    plan.open(128, 480, lowLatency());
    CHECK(plan.blockFrames() == 480);
    CHECK(plan.applyRenderBlock(480) == 480 + 32);
}
