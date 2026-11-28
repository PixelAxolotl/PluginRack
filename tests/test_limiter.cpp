#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "pluginrack/DspMath.h"
#include "pluginrack/LimiterEffect.h"
#include "pluginrack/SceneCodec.h"

TEST_CASE("limiter clamps above ceiling") {
    auto effect = pluginrack::makeDefaultLimiterEffect();
    effect.ceilingDb = -1.0f;
    effect.inputGainDb = 12.0f;
    effect.lookaheadMs = 2.0f;
    pluginrack::LimiterProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    std::vector<float> L(48000, 0.9f), R(48000, 0.9f);
    p.process(L.data(), R.data(), (int)L.size());
    float peak = 0.0f;
    for (int i = 10000; i < 20000; ++i) peak = std::max(peak, std::fabs(L[(std::size_t)i]));
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    REQUIRE(peak <= ceiling * 1.02f);
    REQUIRE(p.gainReductionDb() > 0.0f);
}

TEST_CASE("limiter detects inter-sample peaks") {
    // fs/4 sine at 45 degrees: every sample reads 0.7071, but the true peak
    // between samples is 1.0. A sample-peak detector would not move; the 4x
    // oversampled detector must open the gain.
    auto effect = pluginrack::makeDefaultLimiterEffect();
    effect.ceilingDb = -1.0f;
    effect.lookaheadMs = 1.5f;
    pluginrack::LimiterProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    constexpr int frames = 4800;
    constexpr double twoPi = 2.0 * pluginrack::dsp::kPi;
    std::vector<float> L(frames);
    std::vector<float> R(frames);
    for (int i = 0; i < frames; ++i) {
        const float value = static_cast<float>(
            std::sin(twoPi * 12000.0 * i / 48000.0 + twoPi / 8.0));
        L[static_cast<std::size_t>(i)] = value;
        R[static_cast<std::size_t>(i)] = value;
    }
    p.process(L.data(), R.data(), frames);
    REQUIRE(p.gainReductionDb() > 0.25f);
    for (const float sample : L) REQUIRE(std::isfinite(sample));
}

TEST_CASE("limiter passes quiet audio") {
    auto effect = pluginrack::makeDefaultLimiterEffect();
    pluginrack::LimiterProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    std::vector<float> L(4800, 0.1f), R(4800, 0.1f);
    p.process(L.data(), R.data(), (int)L.size());
    REQUIRE(L.back() == Catch::Approx(0.1f).epsilon(0.05));
}

TEST_CASE("limiter effect settings serialize") {
    pluginrack::Scene scene;
    pluginrack::Channel channel;
    channel.id = 1;
    channel.limiterEffects = {pluginrack::makeDefaultLimiterEffect(4)};
    channel.chain = {{pluginrack::ChainItemType::Limiter, 4}};
    scene.channels.push_back(channel);
    scene.outputs.push_back({1, "Out"});
    const auto json = pluginrack::toJson(scene);
    REQUIRE(json.find("Limiter") != std::string::npos);
    const auto loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels.front().limiterEffects.front().id == 4);
}

TEST_CASE("limiter recovers from a non-finite sample") {
    // The poison sample used to be written straight into the look-ahead ring
    // and re-emerged a block later, so the strip output stayed NaN forever.
    auto effect = pluginrack::makeDefaultLimiterEffect();
    pluginrack::LimiterProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);

    std::vector<float> L(256, 0.0f);
    std::vector<float> R(256, 0.0f);
    L[0] = std::numeric_limits<float>::quiet_NaN();
    R[3] = -std::numeric_limits<float>::infinity();
    p.process(L.data(), R.data(), 256);
    for (int i = 0; i < 256; ++i) {
        REQUIRE(std::isfinite(L[static_cast<std::size_t>(i)]));
        REQUIRE(std::isfinite(R[static_cast<std::size_t>(i)]));
    }
    REQUIRE(std::isfinite(p.gainReductionDb()));

    // Quiet audio afterwards must pass untouched at the look-ahead delay.
    std::vector<float> quiet(4800, 0.1f);
    std::vector<float> quietR = quiet;
    p.process(quiet.data(), quietR.data(), static_cast<int>(quiet.size()));
    REQUIRE(quiet.back() == Catch::Approx(0.1f).epsilon(0.05));
}

TEST_CASE("limiter latency helper reports the look-ahead delay") {
    // Core and app share this one function for compensation math: bypassed
    // strips add nothing, and the look-ahead is clamped exactly like the
    // processor's delay ring.
    auto effect = pluginrack::makeDefaultLimiterEffect(); // lookaheadMs 1.5
    REQUIRE(pluginrack::limiterLatencySamples(effect, 48000.0) == 72);
    effect.bypassed = true;
    REQUIRE(pluginrack::limiterLatencySamples(effect, 48000.0) == 0);
    effect.bypassed = false;
    effect.lookaheadMs = 10.0f;
    REQUIRE(pluginrack::limiterLatencySamples(effect, 96000.0) == 960);
    effect.lookaheadMs = 50.0f; // clamped to the 10 ms cap
    REQUIRE(pluginrack::limiterLatencySamples(effect, 48000.0) == 480);
    effect.lookaheadMs = 0.0f;
    REQUIRE(pluginrack::limiterLatencySamples(effect, 48000.0) == 0);
    // No usable rate means no delay line was ever sized; report nothing.
    effect.lookaheadMs = 1.5f;
    REQUIRE(pluginrack::limiterLatencySamples(effect, 0.0) == 0);
}
