#include <pluginrack/LoudnessMeter.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double amplitude, double freq, double rate = kRate) {
    std::vector<float> block(static_cast<std::size_t>(kBlock));
    static double phase = 0.0;
    for (int i = 0; i < kBlock; ++i) {
        block[static_cast<std::size_t>(i)] =
            static_cast<float>(amplitude * std::sin(phase));
        phase += 2.0 * kPi * freq / rate;
    }
    return block;
}

// Loudness needs its 400 ms window to fill: run a few seconds of blocks.
float settled(pluginrack::LoudnessMeter& meter, double amplitude, double freq,
              double rate = kRate) {
    float lufs = meter.currentLufs();
    const int blocks = static_cast<int>(rate * 3.0 / kBlock);
    for (int b = 0; b < blocks; ++b) {
        const auto block = sine(amplitude, freq, rate);
        lufs = meter.process(block.data(), block.data(), kBlock, rate);
    }
    return lufs;
}

} // namespace

TEST_CASE("a full-scale 1 kHz sine reads near -3 LUFS", "[loudness]") {
    pluginrack::LoudnessMeter meter;
    const float lufs = settled(meter, 1.0, 997.0);
    REQUIRE(lufs == Catch::Approx(-3.0f).margin(1.5f));
}

TEST_CASE("loudness is the same at 96 kHz as at 48 kHz", "[loudness]") {
    // The filters are designed for the active rate. Fixed 48 kHz coefficients
    // halved the weighting corner at 96 kHz and read a different number.
    pluginrack::LoudnessMeter at48;
    pluginrack::LoudnessMeter at96;
    const float a = settled(at48, 1.0, 997.0, 48000.0);
    const float b = settled(at96, 1.0, 997.0, 96000.0);
    REQUIRE(b == Catch::Approx(a).margin(0.5f));
}

TEST_CASE("loudness follows the signal level", "[loudness]") {    pluginrack::LoudnessMeter loud;
    pluginrack::LoudnessMeter quiet;
    const float a = settled(loud, 1.0, 997.0);
    const float b = settled(quiet, 0.1, 997.0);
    // 20 dB down in amplitude is 20 LU down.
    REQUIRE((a - b) == Catch::Approx(20.0f).margin(1.5f));
}

TEST_CASE("silence settles at the floor", "[loudness]") {
    pluginrack::LoudnessMeter meter;
    REQUIRE(meter.currentLufs() == pluginrack::LoudnessMeter::kSilenceLufs);
    const std::vector<float> silence(static_cast<std::size_t>(kBlock), 0.0f);
    for (int b = 0; b < 400; ++b) {
        meter.process(silence.data(), silence.data(), kBlock, kRate);
    }
    REQUIRE(meter.currentLufs() == pluginrack::LoudnessMeter::kSilenceLufs);
}

TEST_CASE("a non-finite block does not poison the meter", "[loudness]") {
    pluginrack::LoudnessMeter meter;
    std::vector<float> bad(static_cast<std::size_t>(kBlock),
                           std::numeric_limits<float>::quiet_NaN());
    meter.process(bad.data(), bad.data(), kBlock, kRate);
    REQUIRE(std::isfinite(meter.currentLufs()));
    const float after = settled(meter, 1.0, 997.0);
    REQUIRE(after == Catch::Approx(-3.0f).margin(1.5f));
}
