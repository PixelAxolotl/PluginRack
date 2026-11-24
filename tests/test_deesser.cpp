#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "pluginrack/DeEsserEffect.h"
#include "pluginrack/SceneCodec.h"

TEST_CASE("deesser band level drives the GR bar") {
    auto effect = pluginrack::makeDefaultDeEsserEffect();
    effect.thresholdDb = -30.0f;
    effect.rangeDb = 12.0f;
    // 440 Hz voice at -6 dBFS + a quiet 6 kHz sibilance at -18 dBFS.
    // Broadband peak is -6 dB (would over-confirm), band peak is -18 dB
    // (exactly the signal the processor reduces). The GR bar must report
    // reduction from the band, not the broadband.
    std::vector<float> L(8192), R(8192);
    for (int i = 0; i < 8192; ++i) {
        const float t = static_cast<float>(i);
        const float mix = 0.5f * std::sin(6.28318f * 440.0f * t / 48000.0f)
                        + 0.125f * std::sin(6.28318f * 6000.0f * t / 48000.0f);
        L[(std::size_t)i] = mix;
        R[(std::size_t)i] = mix;
    }
    float s1L = 0.0f, s2L = 0.0f, s1R = 0.0f, s2R = 0.0f;
    const float bandLevel = pluginrack::deEsserBandLevelDb(L.data(), R.data(), 8192, effect, 48000.0, s1L, s2L, s1R, s2R);
    REQUIRE(bandLevel < -10.0f); // band is quiet, not -6 dB
    REQUIRE(bandLevel > -24.0f); // but it is real sibilance, not silence
    const float reduction = std::abs(pluginrack::deEsserStaticGainDb(bandLevel, effect));
    REQUIRE(reduction > 0.0f);
    REQUIRE(reduction <= 12.0f); // capped by rangeDb
}

TEST_CASE("deesser split cuts sibilance") {
    auto effect = pluginrack::makeDefaultDeEsserEffect();
    effect.thresholdDb = -40.0f;
    effect.rangeDb = 12.0f;
    effect.attackMs = 0.1f;
    effect.releaseMs = 10.0f;
    pluginrack::DeEsserProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    // 6 kHz sine at -6 dBFS
    std::vector<float> L(4800), R(4800);
    for (int i = 0; i < 4800; ++i) {
        L[(std::size_t)i] = 0.5f * std::sin(6.28318f * 6000.0f * (float)i / 48000.0f);
        R[(std::size_t)i] = L[(std::size_t)i];
    }
    p.process(L.data(), R.data(), (int)L.size());
    float peak = 0.0f;
    for (int i = 4000; i < 4800; ++i) peak = std::max(peak, std::fabs(L[(std::size_t)i]));
    REQUIRE(std::isfinite(peak));
    // The band sits ~34 dB over the -40 dB threshold, so the rangeDb 12 dB cap
    // applies: split output = in - band*(1 - 10^(-12/20)). At the band centre
    // the 0 dB-peak band-pass reproduces the tone in phase, leaving
    // 0.5 * 0.2512 ~ 0.126. A loose "< 0.5" would also pass a 1 dB trim.
    REQUIRE(peak == Catch::Approx(0.5f * std::pow(10.0f, -12.0f / 20.0f)).margin(0.02f));
    REQUIRE(p.gainReductionDb() == Catch::Approx(12.0f).margin(0.5f));
}

TEST_CASE("deesser split leaves out-of-band signal untouched") {
    auto effect = pluginrack::makeDefaultDeEsserEffect();
    effect.thresholdDb = -40.0f;
    effect.rangeDb = 12.0f;
    effect.attackMs = 0.1f;
    effect.releaseMs = 10.0f;
    pluginrack::DeEsserProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    // 440 Hz sine at -6 dBFS: far below the 6 kHz sibilance band. With a
    // TRUE band-pass (not a bell), the band contains ~0 of this tone, so
    // (in - band) + band*gain is bit-identical — peak stays 0.5.
    std::vector<float> L(4800), R(4800);
    for (int i = 0; i < 4800; ++i) {
        L[(std::size_t)i] = 0.5f * std::sin(6.28318f * 440.0f * (float)i / 48000.0f);
        R[(std::size_t)i] = L[(std::size_t)i];
    }
    p.process(L.data(), R.data(), (int)L.size());
    float peak = 0.0f;
    for (int i = 4000; i < 4800; ++i) peak = std::max(peak, std::fabs(L[(std::size_t)i]));
    REQUIRE(std::isfinite(peak));
    REQUIRE(peak == Catch::Approx(0.5f).margin(0.02f));
    REQUIRE(p.gainReductionDb() >= 0.0f);
}

TEST_CASE("deesser split mixed in-band and out-of-band") {
    auto effect = pluginrack::makeDefaultDeEsserEffect();
    effect.thresholdDb = -40.0f;
    effect.rangeDb = 12.0f;
    effect.attackMs = 0.1f;
    effect.releaseMs = 10.0f;
    pluginrack::DeEsserProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    // 440 Hz (voice) + 6 kHz (sibilance) at equal levels. Only the 6 kHz
    // should drop; the 440 Hz must pass through untouched.
    std::vector<float> L(4800), R(4800);
    for (int i = 0; i < 4800; ++i) {
        const float t = static_cast<float>(i);
        const float mix = 0.35f * std::sin(6.28318f * 440.0f * t / 48000.0f)
                        + 0.35f * std::sin(6.28318f * 6000.0f * t / 48000.0f);
        L[(std::size_t)i] = mix;
        R[(std::size_t)i] = mix;
    }
    p.process(L.data(), R.data(), (int)L.size());
    REQUIRE(p.gainReductionDb() > 0.0f);
    // Estimate the 440 Hz residual via zero-crossings: it should remain at
    // ~0.35. Check peak does not collapse below the voice level.
    float peak = 0.0f;
    for (int i = 4000; i < 4800; ++i) peak = std::max(peak, std::fabs(L[(std::size_t)i]));
    REQUIRE(std::isfinite(peak));
    REQUIRE(peak > 0.30f);
    REQUIRE(peak < 0.75f);
}

TEST_CASE("deesser wideband still cuts") {
    auto effect = pluginrack::makeDefaultDeEsserEffect();
    effect.mode = pluginrack::DeEsserMode::Wideband;
    effect.thresholdDb = -40.0f;
    effect.rangeDb = 10.0f;
    pluginrack::DeEsserProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    std::vector<float> L(4800, 0.5f), R(4800, 0.5f);
    p.process(L.data(), R.data(), (int)L.size());
    REQUIRE(std::isfinite(L.back()));
    REQUIRE(p.gainReductionDb() > 0.0f);
}

TEST_CASE("deesser effect settings serialize") {
    pluginrack::Scene scene;
    pluginrack::Channel channel;
    channel.id = 1;
    channel.deEsserEffects = {pluginrack::makeDefaultDeEsserEffect(2)};
    channel.chain = {{pluginrack::ChainItemType::DeEsser, 2}};
    scene.channels.push_back(channel);
    scene.outputs.push_back({1, "Out"});
    const auto json = pluginrack::toJson(scene);
    REQUIRE(json.find("DeEsser") != std::string::npos);
    const auto loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels.front().deEsserEffects.front().id == 2);
}

TEST_CASE("de-esser recovers from a non-finite sample") {
    // The band-pass state, envelope and gain all ingested raw samples: a
    // single NaN left the reduction meter pinned and the output poisoned.
    auto effect = pluginrack::makeDefaultDeEsserEffect();
    effect.thresholdDb = -40.0f;
    effect.rangeDb = 12.0f;
    effect.attackMs = 0.1f;
    effect.releaseMs = 10.0f;

    std::vector<float> sibilance(4800), sibilanceR(4800);
    for (int i = 0; i < 4800; ++i) {
        sibilance[static_cast<std::size_t>(i)] =
            0.5f * std::sin(6.28318f * 6000.0f * (float)i / 48000.0f);
        sibilanceR[static_cast<std::size_t>(i)] = sibilance[static_cast<std::size_t>(i)];
    }

    // Reference: a processor that never saw the poison, same effect.
    pluginrack::DeEsserProcessor clean;
    clean.prepare(48000.0);
    clean.setEffect(effect);
    auto cleanLeft = sibilance;
    auto cleanRight = sibilanceR;
    clean.process(cleanLeft.data(), cleanRight.data(), 4800);
    float cleanPeak = 0.0f;
    for (int i = 4000; i < 4800; ++i) {
        cleanPeak = std::max(cleanPeak, std::fabs(cleanLeft[static_cast<std::size_t>(i)]));
    }

    pluginrack::DeEsserProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);
    std::vector<float> poisonedL(256, 0.0f);
    std::vector<float> poisonedR(256, 0.0f);
    poisonedL[0] = std::numeric_limits<float>::quiet_NaN();
    poisonedR[9] = std::numeric_limits<float>::infinity();
    p.process(poisonedL.data(), poisonedR.data(), 256);
    for (int i = 0; i < 256; ++i) {
        REQUIRE(std::isfinite(poisonedL[static_cast<std::size_t>(i)]));
        REQUIRE(std::isfinite(poisonedR[static_cast<std::size_t>(i)]));
    }

    // The same sibilance must now land exactly where the clean sibling puts
    // it: no residue in the band state, envelope or gain.
    auto left = sibilance;
    auto right = sibilanceR;
    p.process(left.data(), right.data(), 4800);
    float peak = 0.0f;
    for (int i = 4000; i < 4800; ++i) {
        peak = std::max(peak, std::fabs(left[static_cast<std::size_t>(i)]));
    }
    REQUIRE(std::isfinite(peak));
    REQUIRE(peak == Catch::Approx(cleanPeak).margin(1.0e-4f));
    REQUIRE(p.gainReductionDb() == Catch::Approx(clean.gainReductionDb()).margin(1.0e-3f));
    // And that shared steady state is the derived one: the band is 34 dB over
    // the threshold, the 12 dB range caps it, so in - band*(1 - 10^(-12/20))
    // leaves 0.5 * 0.2512 ~ 0.126 (a pass-through would keep 0.5).
    REQUIRE(peak == Catch::Approx(0.5f * std::pow(10.0f, -12.0f / 20.0f)).margin(0.02f));
    REQUIRE(peak < 0.16f);
}

