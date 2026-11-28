#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "pluginrack/DeEsserEffect.h"
#include "pluginrack/DspMath.h"
#include "pluginrack/MultibandEffect.h"
#include "pluginrack/TraceData.h"

TEST_CASE("multiband target trace is the real LR4 split response") {
    auto effect = pluginrack::makeDefaultMultibandEffect();
    for (auto& band : effect.bands) band.ratio = 1.0f;
    constexpr std::size_t kBins = 512;
    const auto trace = pluginrack::makeMultibandTargetTrace(effect, kBins);

    REQUIRE(trace.size() == kBins);
    REQUIRE(std::all_of(trace.begin(), trace.end(), [](float value) {
        return std::isfinite(value) && value >= -60.0f && value <= 12.0f;
    }));
    // Pass bands sit at the band trims (0 dB with the default effect).
    const auto peak = *std::max_element(trace.begin(), trace.end());
    REQUIRE(peak > -0.1f);
    REQUIRE(peak < 0.1f);

    // Each LR4 split is -6 dB on both sides at the crossover, so the drawn
    // curve dips there. The old heuristic drew a constant +1.85 dB instead.
    for (const float crossover : effect.crossovers) {
        std::size_t nearest = 0;
        float distance = 1.0e9f;
        for (std::size_t bin = 0; bin < kBins; ++bin) {
            const float hz = 20.0f * std::pow(1000.0f, static_cast<float>(bin)
                                                         / static_cast<float>(kBins - 1));
            const float candidate = std::abs(std::log(hz / crossover));
            if (candidate < distance) { distance = candidate; nearest = bin; }
        }
        INFO("crossover " << crossover << " Hz draws " << trace[nearest] << " dB");
        REQUIRE(trace[nearest] < -5.6f);
        REQUIRE(trace[nearest] > -6.6f);
    }
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

    // The dip has to sit on the detector frequency, not merely exist: the
    // trace is log-spaced over 20 Hz..20 kHz, so the minimum's bin maps back
    // to a frequency and both ends of the graph must stay well above it.
    const auto bin = static_cast<double>(std::distance(trace.begin(), peak));
    const double position = bin / static_cast<double>(trace.size() - 1);
    const double dipHz = 20.0 * std::pow(1000.0, position);
    CHECK(dipHz == Catch::Approx(static_cast<double>(effect.freqHz)).epsilon(0.10));
    CHECK(trace.front() > *peak + 3.0f);
    CHECK(trace.back() > *peak + 3.0f);
}

TEST_CASE("deesser trace follows the engine band-pass, not a Gaussian") {
    // Split mode passes `in + band*(g - 1)`, so the drawn reduction at the
    // band centre is the applied gain and the skirt is the real 2nd-order
    // band-pass. Q = 2 puts the -3 dB points at 4.5 kHz and 7.5 kHz for a
    // 6 kHz detector; the old fixed 0.85/Q log-Gaussian read -5.6 dB there.
    auto effect = pluginrack::makeDefaultDeEsserEffect();
    REQUIRE(effect.freqHz == 6000.0f);
    REQUIRE(effect.q == 2.0f);

    constexpr std::size_t kBins = 2048;
    const auto trace = pluginrack::makeDeEsserGainReductionTrace(effect, 9.0f, kBins);
    const auto frequencyAt = [](std::size_t bin) {
        return 20.0f * std::pow(1000.0f, static_cast<float>(bin)
                                             / static_cast<float>(kBins - 1));
    };
    const auto binAt = [&](float hz) {
        std::size_t nearest = 0;
        float distance = 1.0e9f;
        for (std::size_t bin = 0; bin < kBins; ++bin) {
            const float candidate = std::abs(std::log(frequencyAt(bin) / hz));
            if (candidate < distance) { distance = candidate; nearest = bin; }
        }
        return nearest;
    };

    // Exactly the applied 9 dB at the centre, and far away almost nothing.
    CHECK(trace[binAt(6000.0f)] == Catch::Approx(-9.0).margin(0.05));
    CHECK(trace[binAt(400.0f)] > -0.3f);
    CHECK(trace[binAt(18000.0f)] > -2.0f);

    // The skirt is the engine's own band-pass, not a fixed width: at 4.5 kHz
    // this filter is only ~1.8 dB down (the old 0.85/Q log-Gaussian read
    // -5.6 dB there), and the trace must track it bin for bin.
    const auto band = pluginrack::deEsserBandCoefficients(effect, 48000.0);
    const float gain = std::pow(10.0f, -9.0f / 20.0f);
    for (const float hz : {3000.0f, 4500.0f, 5200.0f, 7500.0f, 9000.0f}) {
        const auto response = pluginrack::dsp::biquadResponse(band, hz, 48000.0);
        const float re = 1.0f + (gain - 1.0f) * response.re;
        const float im = (gain - 1.0f) * response.im;
        const float expected = 20.0f * std::log10(std::sqrt(re * re + im * im));
        INFO(hz << " Hz draws " << trace[binAt(hz)] << " dB, filter law " << expected);
        CHECK(trace[binAt(hz)] == Catch::Approx(expected).margin(0.1f));
    }
    CHECK(trace[binAt(4500.0f)] > -2.5f);
    CHECK(trace[binAt(4500.0f)] < -1.2f);
}

TEST_CASE("deesser wideband trace is flat") {
    // Wideband applies the gain to the whole signal, so the curve has no band
    // shape: the old heuristic drew the split bell in both modes.
    auto effect = pluginrack::makeDefaultDeEsserEffect();
    effect.mode = pluginrack::DeEsserMode::Wideband;
    const auto trace = pluginrack::makeDeEsserGainReductionTrace(effect, 6.0f, 64);
    REQUIRE(trace.size() == 64);
    REQUIRE(std::all_of(trace.begin(), trace.end(), [](float value) {
        return value == Catch::Approx(-6.0f).margin(0.01f);
    }));
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
