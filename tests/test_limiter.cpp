#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

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
