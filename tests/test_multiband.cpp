#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "pluginrack/MultibandEffect.h"
#include "pluginrack/SceneCodec.h"

using pluginrack::MultibandEffect;
using pluginrack::MultibandProcessor;

TEST_CASE("multiband flat passes audio") {
    auto effect = pluginrack::makeDefaultMultibandEffect();
    MultibandProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    // Warm up filter startup, then check steady state near DC with
    // thresholds low enough that the knee cannot engage at -12 dBFS.
    for (auto& b : effect.bands) { b.thresholdDb = -6.0f; b.ratio = 2.0f; }
    p.setEffect(effect);
    std::vector<float> warm(48000, 0.25f), warmR(48000, 0.25f);
    p.process(warm.data(), warmR.data(), (int)warm.size());
    std::vector<float> L(48000, 0.25f), R(48000, 0.25f);
    p.process(L.data(), R.data(), (int)L.size());
    REQUIRE(std::isfinite(L.back()));
    REQUIRE(L.back() == Catch::Approx(0.25f).epsilon(0.35));
}

TEST_CASE("multiband compresses hot low band") {
    auto effect = pluginrack::makeDefaultMultibandEffect();
    effect.bands[0].thresholdDb = -30.0f;
    effect.bands[0].ratio = 8.0f;
    MultibandProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    std::vector<float> L(48000, 0.8f), R(48000, 0.8f);
    p.process(L.data(), R.data(), (int)L.size());
    REQUIRE(L.back() < 0.8f);
    REQUIRE(p.gainReductionDb().size() == 4);
    REQUIRE(p.gainReductionDb()[0] > 0.0f);
}

TEST_CASE("multiband expansion drops quiet band") {
    auto effect = pluginrack::makeDefaultMultibandEffect();
    effect.bands[0].mode = pluginrack::MultibandMode::Expand;
    effect.bands[0].thresholdDb = -10.0f;
    effect.bands[0].ratio = 2.0f;
    MultibandProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    std::vector<float> L(48000, 0.01f), R(48000, 0.01f);
    p.process(L.data(), R.data(), (int)L.size());
    REQUIRE(L.back() < 0.01f);
}

TEST_CASE("multiband effect settings serialize") {
    pluginrack::Scene scene;
    pluginrack::Channel channel;
    channel.id = 1;
    channel.multibandEffects = {pluginrack::makeDefaultMultibandEffect(3)};
    channel.chain = {{pluginrack::ChainItemType::Multiband, 3}};
    scene.channels.push_back(channel);
    scene.outputs.push_back({1, "Out"});
    const auto json = pluginrack::toJson(scene);
    REQUIRE(json.find("Multiband") != std::string::npos);
    const auto loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels.front().multibandEffects.front().id == 3);
}
