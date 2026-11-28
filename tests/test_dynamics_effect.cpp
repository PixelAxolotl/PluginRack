#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "pluginrack/DynamicsEffect.h"
#include "pluginrack/SceneCodec.h"

TEST_CASE("dynamics defaults expose the built-in effects") {
    const auto effects = pluginrack::defaultDynamicsEffects();
    REQUIRE(effects.size() == 2);
    REQUIRE(effects[0].type == pluginrack::DynamicsEffectType::Compressor);
    REQUIRE(effects[1].type == pluginrack::DynamicsEffectType::Gate);
}

TEST_CASE("compressor reduces a sustained signal") {
    pluginrack::DynamicsEffect effect;
    effect.bypassed = false;
    effect.thresholdDb = -18.0f;
    effect.ratio = 4.0f;
    pluginrack::DynamicsProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);
    std::vector<float> left(4800, 1.0f);
    std::vector<float> right(4800, 1.0f);
    processor.process(left.data(), right.data(), static_cast<int>(left.size()));
    REQUIRE(std::fabs(left.back()) < 0.6f);
    REQUIRE(std::isfinite(left.back()));
}

TEST_CASE("gate attenuates by its range instead of muting") {
    pluginrack::DynamicsEffect effect;
    effect.type = pluginrack::DynamicsEffectType::Gate;
    effect.bypassed = false;
    effect.thresholdDb = -20.0f;
    effect.ratio = 10.0f; // gate Range in dB
    effect.attackMs = 0.1f;
    effect.releaseMs = 10.0f;
    pluginrack::DynamicsProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);
    std::vector<float> left(48000, 0.01f); // -40 dBFS, well under the threshold
    std::vector<float> right(48000, 0.01f);
    processor.process(left.data(), right.data(), static_cast<int>(left.size()));
    // 10 dB down, not muted: 0.01 * 10^(-10/20).
    REQUIRE(left.back() == Catch::Approx(0.01f * 0.31623f).epsilon(0.02));
}

TEST_CASE("dynamics effect settings serialize") {
    pluginrack::DynamicsEffect effect;
    effect.id = 9;
    effect.type = pluginrack::DynamicsEffectType::Gate;
    effect.bypassed = true;
    effect.thresholdDb = -2.0f;
    effect.inputGainDb = 2.0f;
    effect.kneeDb = 6.0f;
    pluginrack::Scene scene;
    pluginrack::Channel channel;
    channel.id = 1;
    channel.effects = {effect};
    scene.channels.push_back(channel);
    const auto json = pluginrack::toJson(scene);
    REQUIRE(json.find("effects") != std::string::npos);
    const auto loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels.front().effects.front() == effect);
}

TEST_CASE("dynamics recovers from a non-finite sample") {
    // One NaN sample used to latch the envelope and gain forever: every later
    // block came out silent or poisoned. Input sanitising plus state flushing
    // must let the compressor keep working on the audio that follows.
    pluginrack::DynamicsEffect effect;
    effect.bypassed = false;
    effect.thresholdDb = -18.0f;
    effect.ratio = 4.0f;
    pluginrack::DynamicsProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    std::vector<float> left(256, 0.0f);
    std::vector<float> right(256, 0.0f);
    left[0] = std::numeric_limits<float>::quiet_NaN();
    right[4] = -std::numeric_limits<float>::infinity();
    processor.process(left.data(), right.data(), 256);
    for (int i = 0; i < 256; ++i) {
        REQUIRE(std::isfinite(left[static_cast<std::size_t>(i)]));
        REQUIRE(std::isfinite(right[static_cast<std::size_t>(i)]));
    }

    std::vector<float> loud(4800, 1.0f);
    std::vector<float> loudR = loud;
    processor.process(loud.data(), loudR.data(), static_cast<int>(loud.size()));
    REQUIRE(std::isfinite(loud.back()));
    REQUIRE(std::fabs(loud.back()) < 0.6f);
}

namespace {

pluginrack::DynamicsEffect gateSettings() {
    pluginrack::DynamicsEffect effect;
    effect.type = pluginrack::DynamicsEffectType::Gate;
    effect.bypassed = false;
    effect.thresholdDb = -20.0f;
    effect.ratio = 40.0f;
    effect.attackMs = 0.1f;
    effect.releaseMs = 5.0f;
    effect.kneeDb = 0.0f;
    return effect;
}

std::vector<float> runGate(const pluginrack::DynamicsEffect& effect,
                           float loudLevel, float quietLevel) {
    constexpr int loudFrames = 4800;
    constexpr int quietFrames = 14400;
    pluginrack::DynamicsProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);
    std::vector<float> left(loudFrames + quietFrames, quietLevel);
    std::fill(left.begin(), left.begin() + loudFrames, loudLevel);
    std::vector<float> right = left;
    processor.process(left.data(), right.data(), static_cast<int>(left.size()));
    return left;
}

pluginrack::DynamicsEffect compressorSettings() {
    pluginrack::DynamicsEffect effect;
    effect.bypassed = false;
    effect.thresholdDb = -20.0f;
    effect.ratio = 4.0f;
    effect.attackMs = 1.0f;
    effect.releaseMs = 5.0f;
    effect.kneeDb = 0.0f;
    return effect;
}

float tailPeak(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (std::size_t i = samples.size() / 2; i < samples.size(); ++i)
        peak = std::max(peak, std::fabs(samples[i]));
    return peak;
}

} // namespace

TEST_CASE("gate hold keeps the gate open then closes it") {
    auto effect = gateSettings();
    effect.holdMs = 100.0f; // 4800 samples at 48 kHz

    const auto held = runGate(effect, 0.5f, 0.001f);
    // Mid-hold the tail passes untouched even though it is far under threshold.
    REQUIRE(held[static_cast<std::size_t>(4800 + 2400)] >= 0.0009f);
    // Hold expired: the gate closes and the tail is Range dB down.
    REQUIRE(held[static_cast<std::size_t>(4800 + 9600)] < 5.0e-5f);
    REQUIRE(held[static_cast<std::size_t>(4800 + 9600)] > 1.0e-7f);

    effect.holdMs = 0.0f;
    const auto immediate = runGate(effect, 0.5f, 0.001f);
    REQUIRE(immediate[static_cast<std::size_t>(4800 + 2400)] < 5.0e-5f);
}

TEST_CASE("gate hysteresis keeps the gate open inside the hysteresis band") {
    auto effect = gateSettings();

    effect.hysteresisDb = 0.0f;
    const auto hard = runGate(effect, 0.5f, 0.0501f); // -26 dBFS
    // Threshold is -20 dB, so the tail closes immediately.
    REQUIRE(hard.back() < 0.0025f);

    effect.hysteresisDb = 12.0f;
    const auto soft = runGate(effect, 0.5f, 0.0501f);
    // Closing needs -32 dBFS now; -26 dBFS stays in the open band.
    REQUIRE(soft.back() > 0.04f);
    REQUIRE(soft.back() > hard.back() * 10.0f);
}

TEST_CASE("gate side-chain high-pass changes low-frequency detection") {
    const auto run = [](float sidechainHz) {
        auto effect = gateSettings();
        effect.releaseMs = 10.0f;
        effect.sidechainHz = sidechainHz;
        constexpr int frames = 24000;
        constexpr double twoPi = 2.0 * pluginrack::dsp::kPi;
        pluginrack::DynamicsProcessor processor;
        processor.prepare(48000.0);
        processor.setEffect(effect);
        std::vector<float> left(frames);
        std::vector<float> right(frames);
        for (int i = 0; i < frames; ++i) {
            const float value = 0.8f *
                static_cast<float>(std::sin(twoPi * 50.0 * i / 48000.0));
            left[static_cast<std::size_t>(i)] = value;
            right[static_cast<std::size_t>(i)] = value;
        }
        processor.process(left.data(), right.data(), frames);
        return tailPeak(left);
    };

    const float unfiltered = run(0.0f);
    const float filtered = run(500.0f);
    // 0.8 at 50 Hz is well over -20 dBFS; the same tone through a 500 Hz
    // side-chain high-pass is far under it, so the gate stays closed.
    REQUIRE(unfiltered > 0.5f);
    REQUIRE(filtered < unfiltered * 0.1f);
}

TEST_CASE("RMS detector produces a different gain than peak") {
    const auto run = [](bool rmsDetector) {
        auto effect = compressorSettings();
        effect.attackMs = 0.1f;
        effect.releaseMs = 50.0f;
        effect.rmsDetector = rmsDetector;
        constexpr int frames = 4800;
        constexpr double twoPi = 2.0 * pluginrack::dsp::kPi;
        pluginrack::DynamicsProcessor processor;
        processor.prepare(48000.0);
        processor.setEffect(effect);
        std::vector<float> left(frames);
        std::vector<float> right(frames);
        for (int i = 0; i < frames; ++i) {
            const float value =
                static_cast<float>(std::sin(twoPi * 1000.0 * i / 48000.0));
            left[static_cast<std::size_t>(i)] = value;
            right[static_cast<std::size_t>(i)] = value;
        }
        processor.process(left.data(), right.data(), frames);
        return tailPeak(left);
    };

    const float peak = run(false);
    const float rms = run(true);
    // A sine's RMS is 0.707 of its peak, so the same threshold compresses
    // about 3 dB less: rms gain = 0.707^(1/ratio-1) of the peak gain.
    REQUIRE(peak > 0.05f);
    REQUIRE(rms > peak * 1.1f);
    REQUIRE(rms < peak * 1.6f);
}

TEST_CASE("compressor mix blends dry and processed") {
    const auto run = [](float mixPercent) {
        auto effect = compressorSettings();
        effect.mixPercent = mixPercent;
        constexpr int frames = 4800;
        pluginrack::DynamicsProcessor processor;
        processor.prepare(48000.0);
        processor.setEffect(effect);
        std::vector<float> left(frames, 0.5f);
        std::vector<float> right(frames, 0.5f);
        processor.process(left.data(), right.data(), frames);
        return left;
    };

    const auto dry = run(0.0f);
    for (const float sample : dry) REQUIRE(sample == 0.5f);

    const auto wet = run(100.0f);
    const float expectedWet = 0.5f * std::pow(5.0f, 0.25f - 1.0f);
    REQUIRE(wet.back() == Catch::Approx(expectedWet).epsilon(0.01));
    REQUIRE(std::fabs(wet.back() - 0.5f) > 0.1f);

    const auto half = run(50.0f);
    REQUIRE(half.back() == Catch::Approx((0.5f + wet.back()) * 0.5f).epsilon(0.01));
}


