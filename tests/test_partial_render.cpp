#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pluginrack/RenderSlicer.h>

namespace {

using pluginrack::SliceLayout;

} // namespace

TEST_CASE("slices cover the whole block exactly", "[render][slice]") {
    const SliceLayout layout = pluginrack::planSlices(256, 4);
    CHECK(layout.count == 4);
    CHECK(layout.sliceFrames == 64);
    CHECK(layout.remainderFrames == 0);
    CHECK(layout.totalFrames() == 256);

    int covered = 0;
    for (int i = 0; i < layout.count; ++i) {
        CHECK(layout.offsetAt(i) == i * 64);
        CHECK(layout.framesAt(i) == 64);
        covered += layout.framesAt(i);
    }
    CHECK(covered == 256);
}

TEST_CASE("a remainder lands in the last slice", "[render][slice]") {
    const SliceLayout layout = pluginrack::planSlices(250, 4);
    CHECK(layout.count == 4);
    CHECK(layout.sliceFrames == 62);
    CHECK(layout.remainderFrames == 2);
    CHECK(layout.framesAt(0) == 62);
    CHECK(layout.framesAt(3) == 64);
    CHECK(layout.offsetAt(3) == 186);
    CHECK(layout.totalFrames() == 250);
}

TEST_CASE("power-of-two blocks get power-of-two slices", "[render][slice]") {
    CHECK(pluginrack::planSlices(512, 4).sliceFrames == 128);
    CHECK(pluginrack::planSlices(128, 4).sliceFrames == 32);
    CHECK(pluginrack::planSlices(64, 4).sliceFrames == 16);
    // A block smaller than the slice count still produces one frame per slice.
    const SliceLayout tiny = pluginrack::planSlices(2, 4);
    CHECK(tiny.count == 2);
    CHECK(tiny.sliceFrames == 1);
    CHECK(tiny.totalFrames() == 2);
}

TEST_CASE("the deadline budget is a fraction of one block period",
          "[render][slice]") {
    // 128 frames at 48 kHz is 2.667 ms; 80 % leaves 2.133 ms.
    const double budget = pluginrack::deadlineBudgetMs(128, 48000.0, 0.8);
    CHECK(budget == Catch::Approx(2.1333).margin(0.001));
    CHECK_FALSE(pluginrack::deadlineExceededMs(1.0, budget));
    CHECK(pluginrack::deadlineExceededMs(budget, budget));
    CHECK(pluginrack::deadlineExceededMs(budget + 0.1, budget));

    // A zero block or rate disables the gate rather than dividing by zero.
    CHECK(pluginrack::deadlineBudgetMs(0, 48000.0) == 0.0);
    CHECK(pluginrack::deadlineBudgetMs(128, 0.0) == 0.0);
    CHECK_FALSE(pluginrack::deadlineExceededMs(100.0, 0.0));
}
