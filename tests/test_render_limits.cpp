// PR-03: the offline renderer must reject oversize scenes and renders.
#include <catch2/catch_test_macros.hpp>

#include "test_render_limits.h" // re-exports tools/render_cli/limits.h

TEST_CASE("render_limits: scene size cap matches the GUI loader", "[render][limits]") {
    using namespace render_limits;
    CHECK(sceneWithinLimit(0));
    CHECK(sceneWithinLimit(8 * 1024 * 1024));
    CHECK_FALSE(sceneWithinLimit(8 * 1024 * 1024 + 1));
    CHECK_FALSE(sceneWithinLimit(-1));
}

TEST_CASE("render_limits: seconds cap", "[render][limits]") {
    using namespace render_limits;
    CHECK(secondsWithinLimit(2.0, 48000.0));
    CHECK(secondsWithinLimit(kMaxSeconds, 48000.0));
    CHECK_FALSE(secondsWithinLimit(0.0, 48000.0));
    CHECK_FALSE(secondsWithinLimit(-1.0, 48000.0));
    CHECK_FALSE(secondsWithinLimit(kMaxSeconds + 1.0, 48000.0));
}

TEST_CASE("render_limits: total output sample budget", "[render][limits]") {
    using namespace render_limits;
    CHECK(outputWithinLimit(48000.0, 2));
    CHECK(outputWithinLimit(kMaxOutputSamples / 4.0, 2));
    CHECK_FALSE(outputWithinLimit(kMaxOutputSamples / 4.0 + 1.0, 2));
    CHECK_FALSE(outputWithinLimit(1e18, 64));
}
