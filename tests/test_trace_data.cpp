#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "pluginrack/DeEsserEffect.h"
#include "pluginrack/MultibandEffect.h"
#include "pluginrack/TraceData.h"

TEST_CASE("multiband target trace follows EQ-shaped bands") {
    auto effect = pluginrack::makeDefaultMultibandEffect();
    const auto trace = pluginrack::makeMultibandTargetTrace(effect, 64);

    REQUIRE(trace.size() == 64);
    REQUIRE(std::all_of(trace.begin(), trace.end(), [](float value) {
        return std::isfinite(value) && value >= -60.0f && value <= 12.0f;
    }));
    const auto peak = *std::max_element(trace.begin(), trace.end());
    REQUIRE(peak > 0.0f);
    // Bands are flat (1.85 dB everywhere with the default effect) — the
    // trace follows the band level, it does not hump at crossovers.
    REQUIRE(peak == *std::min_element(trace.begin(), trace.end()));
}

TEST_CASE("multiband gain-change trace maps all bands to negative dB") {
    auto effect = pluginrack::makeDefaultMultibandEffect();
    const auto trace = pluginrack::makeMultibandGainChangeTrace(effect, {2.0f, 4.0f, 6.0f, 8.0f}, 64);

    REQUIRE(trace.size() == 64);
    REQUIRE(*std::min_element(trace.begin(), trace.end()) < -7.0f);
    REQUIRE(std::all_of(trace.begin(), trace.end(), [](float value) {
        return std::isfinite(value) && value <= 0.0f && value >= -60.0f;
    }));
}

TEST_CASE("deesser reduction trace is centered on detector frequency") {
    auto effect = pluginrack::makeDefaultDeEsserEffect();
    const auto trace = pluginrack::makeDeEsserGainReductionTrace(effect, 9.0f, 128);

    REQUIRE(trace.size() == 128);
    const auto peak = std::min_element(trace.begin(), trace.end());
    REQUIRE(*peak < -8.0f);
    REQUIRE(std::isfinite(*peak));
}

TEST_CASE("trace shaping clamps non-finite and mismatched input") {
    auto effect = pluginrack::makeDefaultMultibandEffect();
    const auto trace = pluginrack::makeMultibandGainChangeTrace(
        effect,
        {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()},
        8);

    REQUIRE(trace.size() == 8);
    REQUIRE(std::all_of(trace.begin(), trace.end(), [](float value) {
        return std::isfinite(value) && value <= 0.0f && value >= -60.0f;
    }));
}

TEST_CASE("level history rolls over in chronological order") {
    pluginrack::LevelHistory history(3);
    history.push(-30.0f, -1.0f, -31.0f);
    history.push(-20.0f, -2.0f, -22.0f);
    history.push(-10.0f, -3.0f, -13.0f);
    history.push(-5.0f, -4.0f, -9.0f);

    std::vector<float> input(3), reduction(3), output(3);
    history.copyOrderedTo(input, reduction, output);
    REQUIRE(input == std::vector<float>{-20.0f, -10.0f, -5.0f});
    REQUIRE(reduction == std::vector<float>{-2.0f, -3.0f, -4.0f});
    REQUIRE(output == std::vector<float>{-22.0f, -13.0f, -9.0f});
}
