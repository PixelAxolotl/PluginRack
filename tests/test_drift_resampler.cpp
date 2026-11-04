#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

#include "pluginrack/DriftResampler.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDt = 0.01;      // 10 ms control interval
constexpr double kTarget = 480.0; // two 10 ms buffers at 48 kHz
constexpr double kMaxCorrection = 0.005;

pluginrack::DriftResampler makeResampler() {
    pluginrack::DriftResampler resampler;
    resampler.configure(pluginrack::DriftResampler::Config{});
    resampler.reset();
    return resampler;
}

int wrapIndex(int index, int capacity) {
    const int wrapped = index % capacity;
    return wrapped < 0 ? wrapped + capacity : wrapped;
}

// The two-tap reader this module replaces, kept here so the quality test can
// compare kernels without widening the production API.
double readLinear(const float* ring, int capacity, int index, double phase) {
    const double a = static_cast<double>(ring[wrapIndex(index, capacity)]);
    const double b = static_cast<double>(ring[wrapIndex(index + 1, capacity)]);
    return a + (b - a) * phase;
}

} // namespace

TEST_CASE("zero fill error holds the ratio at unity") {
    auto resampler = makeResampler();
    for (int update = 0; update < 600; ++update) {
        REQUIRE(resampler.updateRatio(0.0, kTarget, kDt) == 1.0);
    }
    REQUIRE(resampler.integral() == 0.0);
    REQUIRE(resampler.ratio() == 1.0);
}

TEST_CASE("sustained positive fill error raises the ratio inside the limit") {
    auto resampler = makeResampler();
    double ratio = 1.0;
    for (int update = 0; update < 400; ++update) {
        ratio = resampler.updateRatio(120.0, kTarget, kDt);
        REQUIRE(ratio > 1.0);
        REQUIRE(ratio <= 1.0 + kMaxCorrection);
    }
    REQUIRE(ratio == Catch::Approx(1.0 + kMaxCorrection));
}

TEST_CASE("sustained negative fill error lowers the ratio inside the limit") {
    auto resampler = makeResampler();
    double ratio = 1.0;
    for (int update = 0; update < 400; ++update) {
        ratio = resampler.updateRatio(-120.0, kTarget, kDt);
        REQUIRE(ratio < 1.0);
        REQUIRE(ratio >= 1.0 - kMaxCorrection);
    }
    REQUIRE(ratio == Catch::Approx(1.0 - kMaxCorrection));
}

TEST_CASE("integral leak returns the ratio to unity within five seconds") {
    auto resampler = makeResampler();
    // Drive the integrator into its clamp, then remove the error entirely.
    for (int update = 0; update < 200; ++update) {
        resampler.updateRatio(4.0 * kTarget, kTarget, kDt);
    }
    REQUIRE(resampler.integral() == Catch::Approx(0.004).margin(1e-12));

    for (int update = 0; update < 500; ++update) {
        resampler.updateRatio(0.0, kTarget, kDt);
    }
    REQUIRE(resampler.integral() == Catch::Approx(0.0).margin(1e-4));
    REQUIRE(resampler.ratio() == Catch::Approx(1.0).margin(1e-4));
}

TEST_CASE("non-positive dt is clamped instead of exploding") {
    auto resampler = makeResampler();

    const double fromZero = resampler.updateRatio(200.0, kTarget, 0.0);
    REQUIRE(std::isfinite(fromZero));
    REQUIRE(fromZero <= 1.0 + kMaxCorrection);

    const double fromNegative = resampler.updateRatio(200.0, kTarget, -2.0);
    REQUIRE(std::isfinite(fromNegative));
    REQUIRE(fromNegative >= 1.0 - kMaxCorrection);
    REQUIRE(fromNegative <= 1.0 + kMaxCorrection);

    const double fromHuge = resampler.updateRatio(200.0, kTarget, 1.0e6);
    REQUIRE(std::isfinite(fromHuge));
    REQUIRE(fromHuge <= 1.0 + kMaxCorrection);

    // A zero target must not divide by zero.
    REQUIRE(std::isfinite(resampler.updateRatio(0.0, 0.0, kDt)));
}

TEST_CASE("non-finite inputs do not poison the ratio") {
    auto resampler = makeResampler();
    const double settled = resampler.updateRatio(200.0, kTarget, kDt);
    REQUIRE(settled > 1.0);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    REQUIRE(resampler.updateRatio(nan, kTarget, kDt) == settled);
    REQUIRE(resampler.updateRatio(200.0, nan, kDt) == settled);
    REQUIRE(resampler.updateRatio(200.0, kTarget, nan) == settled);
    REQUIRE(resampler.updateRatio(inf, kTarget, kDt) == settled);
    REQUIRE(resampler.updateRatio(200.0, inf, kDt) == settled);

    REQUIRE(std::isfinite(resampler.ratio()));
    REQUIRE(resampler.ratio() == Catch::Approx(settled));
}

TEST_CASE("unity ratio reproduces the ring from the start index") {
    constexpr int capacity = 16;
    constexpr int frames = 10;
    constexpr int start = 3;

    std::array<float, capacity> ring{};
    for (int i = 0; i < capacity; ++i) {
        ring[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.25f - 1.0f;
    }

    std::array<float, frames> dest{};
    pluginrack::DriftResampler::interpolateCubic(ring.data(), capacity, start, frames, 1.0,
                                                 dest.data());
    for (int frame = 0; frame < frames; ++frame) {
        const auto expected = ring[static_cast<std::size_t>((start + frame) % capacity)];
        REQUIRE(dest[static_cast<std::size_t>(frame)] == Catch::Approx(expected).margin(1e-6));
    }
    REQUIRE(dest[0] == ring[static_cast<std::size_t>(start)]);
}

TEST_CASE("cubic read wraps past the end of the ring") {
    constexpr int capacity = 8;
    constexpr int frames = 6;
    constexpr int start = 5;

    std::array<float, capacity> ring{};
    for (int i = 0; i < capacity; ++i) ring[static_cast<std::size_t>(i)] = static_cast<float>(i);

    // startIndex + frames * ratio = 11 > capacity, so the tail comes from the head.
    std::array<float, frames> dest{};
    pluginrack::DriftResampler::interpolateCubic(ring.data(), capacity, start, frames, 1.0,
                                                 dest.data());
    for (int frame = 0; frame < frames; ++frame) {
        const auto expected = ring[static_cast<std::size_t>((start + frame) % capacity)];
        REQUIRE(dest[static_cast<std::size_t>(frame)] == Catch::Approx(expected).margin(1e-6));
    }
    REQUIRE(dest[5] == Catch::Approx(ring[2]).margin(1e-6));
}

TEST_CASE("cubic interpolation beats linear on a 1 kHz sine") {
    constexpr int capacity = 192;  // four 1 kHz periods at 48 kHz, so wrapping is seamless
    constexpr int frames = 192;
    constexpr int start = 5;
    constexpr double ratio = 1.003;

    std::array<float, capacity> ring{};
    for (int i = 0; i < capacity; ++i) {
        ring[static_cast<std::size_t>(i)] = static_cast<float>(
            std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / 48000.0));
    }

    std::array<float, frames> cubic{};
    pluginrack::DriftResampler::interpolateCubic(ring.data(), capacity, start, frames, ratio,
                                                 cubic.data());

    double cubicError = 0.0;
    double linearError = 0.0;
    for (int frame = 0; frame < frames; ++frame) {
        const double position = static_cast<double>(start) + static_cast<double>(frame) * ratio;
        const double reference = std::sin(2.0 * kPi * 1000.0 * position / 48000.0);
        const auto index = static_cast<int>(std::floor(position));
        const double phase = position - static_cast<double>(index);
        const double linear = readLinear(ring.data(), capacity, index, phase);
        cubicError = std::max(cubicError,
                              std::abs(static_cast<double>(cubic[static_cast<std::size_t>(frame)])
                                       - reference));
        linearError = std::max(linearError, std::abs(linear - reference));
    }

    REQUIRE(linearError > 1.0e-3);
    REQUIRE(cubicError < 0.25 * linearError);
}

TEST_CASE("capacity below four writes zeros and reads nothing") {
    const std::array<float, 3> ring{1.0f, 2.0f, 3.0f};
    std::array<float, 8> dest{};
    dest.fill(9.0f);
    pluginrack::DriftResampler::interpolateCubic(ring.data(), 3, 0, 8, 1.0, dest.data());
    for (const float value : dest) REQUIRE(value == 0.0f);

    // No ring at all, and a non-finite ratio, both degrade to silence.
    dest.fill(9.0f);
    pluginrack::DriftResampler::interpolateCubic(nullptr, 0, 0, 8, 1.0, dest.data());
    for (const float value : dest) REQUIRE(value == 0.0f);

    dest.fill(9.0f);
    pluginrack::DriftResampler::interpolateCubic(
        ring.data(), 3, 0, 8, std::numeric_limits<double>::quiet_NaN(), dest.data());
    for (const float value : dest) REQUIRE(value == 0.0f);
}

TEST_CASE("non-finite ring samples collapse to zeros locally") {
    constexpr int capacity = 8;
    constexpr int frames = 6;
    constexpr int start = 2;

    std::array<float, capacity> ring{};
    for (int i = 0; i < capacity; ++i) {
        ring[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.1f;
    }
    ring[3] = std::numeric_limits<float>::quiet_NaN();

    std::array<float, frames> dest{};
    pluginrack::DriftResampler::interpolateCubic(ring.data(), capacity, start, frames, 1.0,
                                                 dest.data());

    // Frames whose four taps include ring[3] go silent; the rest are intact.
    REQUIRE(dest[0] == 0.0f);
    REQUIRE(dest[1] == 0.0f);
    REQUIRE(dest[2] == 0.0f);
    REQUIRE(dest[3] == Catch::Approx(ring[5]).margin(1e-6));
    REQUIRE(dest[4] == Catch::Approx(ring[6]).margin(1e-6));
    REQUIRE(dest[5] == Catch::Approx(ring[7]).margin(1e-6));
}
