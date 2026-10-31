#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
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
