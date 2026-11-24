#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include "pluginrack/MultibandEffect.h"
#include "pluginrack/SceneCodec.h"

using pluginrack::MultibandEffect;
using pluginrack::MultibandProcessor;

namespace {

// Steady-state RMS(out)/RMS(in) from `begin` onward.
double rmsGain(const std::vector<float>& in, const std::vector<float>& out, std::size_t begin) {
    double sumIn = 0.0;
    double sumOut = 0.0;
    for (std::size_t i = begin; i < in.size(); ++i) {
        sumIn += static_cast<double>(in[i]) * in[i];
        sumOut += static_cast<double>(out[i]) * out[i];
    }
    return sumIn > 0.0 ? std::sqrt(sumOut / sumIn) : 0.0;
}

std::vector<float> sine(int frames, float freqHz, float amplitude) {
    std::vector<float> out(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        out[static_cast<std::size_t>(i)] = amplitude * std::sin(
            2.0f * 3.14159265f * freqHz * static_cast<float>(i) / 48000.0f);
    }
    return out;
}

} // namespace

TEST_CASE("multiband flat passes audio") {
    auto effect = pluginrack::makeDefaultMultibandEffect();
    MultibandProcessor p;
    p.prepare(48000.0);
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

TEST_CASE("multiband reserves scratch for every crossover") {
    // prepare() runs before setEffect(), so the scratch was sized for the
    // default empty crossover list. A six-band effect then wrote past the
    // buffers on the audio thread.
    auto effect = pluginrack::makeDefaultMultibandEffect();
    effect.bands.resize(6);
    effect.crossovers = {200.0f, 800.0f, 3000.0f, 8000.0f, 12000.0f};

    MultibandProcessor p;
    p.prepare(48000.0, 256);
    p.setEffect(effect);

    std::vector<float> L(256, 0.25f), R(256, 0.25f);
    p.process(L.data(), R.data(), 256);
    REQUIRE(std::isfinite(L.back()));
    REQUIRE(std::isfinite(R.back()));
    REQUIRE(p.gainReductionDb().size() == 6);
}

TEST_CASE("multiband holds unity gain across every crossover") {
    // Flat ratio so no band can compress, then check the split itself sums
    // back to the input. The old LP0+HP0 telescoping measured ~ -93.6 dB at
    // the first crossover (250 Hz); ±0.5 dB is the pass.
    auto effect = pluginrack::makeDefaultMultibandEffect();
    for (auto& b : effect.bands) {
        b.ratio = 1.0f;
    }
    MultibandProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);

    constexpr int kFrames = 32768;
    for (const float freq : {125.0f, 250.0f, 2000.0f, 6000.0f}) {
        const auto orig = sine(kFrames, freq, 0.5f);
        auto L = orig;
        auto R = orig;
        p.process(L.data(), R.data(), kFrames);
        const double gain = rmsGain(orig, L, static_cast<std::size_t>(kFrames) / 2);
        REQUIRE(gain > 0.9441); // -0.5 dB
        REQUIRE(gain < 1.0593); // +0.5 dB
        REQUIRE(std::isfinite(L.back()));
        REQUIRE(std::isfinite(R.back()));
    }
}

TEST_CASE("multiband with a single band passes audio through") {
    // bands == 1 means an empty crossover list; the old split loop never ran
    // and band 0 came out all zeros.
    auto effect = pluginrack::makeDefaultMultibandEffect();
    effect.bands.resize(1);
    effect.crossovers.clear();
    effect.bands[0].ratio = 1.0f;
    MultibandProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);

    constexpr int kFrames = 4800;
    const auto orig = sine(kFrames, 1000.0f, 0.25f);
    auto L = orig;
    auto R = orig;
    p.process(L.data(), R.data(), kFrames);
    REQUIRE(rmsGain(orig, L, static_cast<std::size_t>(kFrames) / 2)
            == Catch::Approx(1.0).margin(0.001));
    for (int i = 1000; i < kFrames; ++i) {
        REQUIRE(L[static_cast<std::size_t>(i)]
                == Catch::Approx(orig[static_cast<std::size_t>(i)]).margin(1.0e-5));
    }
}

TEST_CASE("multiband grows scratch for more than five crossovers after prepare") {
    // prepare() sized the planes for the validator's five-crossover cap; an
    // effect set afterwards with more must re-grow hpL_/hpR_, not overrun them.
    MultibandProcessor p;
    p.prepare(48000.0, 256);
    auto effect = pluginrack::makeDefaultMultibandEffect();
    effect.bands.resize(8);
    effect.crossovers = {120.0f, 350.0f, 900.0f, 1800.0f, 3600.0f, 7200.0f, 11000.0f};
    p.setEffect(effect);

    std::vector<float> L(256, 0.25f), R(256, 0.25f);
    p.process(L.data(), R.data(), 256);
    REQUIRE(std::isfinite(L.back()));
    REQUIRE(std::isfinite(R.back()));
    REQUIRE(p.gainReductionDb().size() == 8);
}

TEST_CASE("multiband recovers from a non-finite sample") {
    // One NaN/Inf sample used to poison the crossover states and every band
    // envelope: the whole processor output stayed NaN for the session.
    auto effect = pluginrack::makeDefaultMultibandEffect();
    MultibandProcessor p;
    p.prepare(48000.0);
    p.setEffect(effect);

    std::vector<float> L(256, 0.0f), R(256, 0.0f);
    L[0] = std::numeric_limits<float>::quiet_NaN();
    R[7] = std::numeric_limits<float>::infinity();
    p.process(L.data(), R.data(), 256);
    for (int i = 0; i < 256; ++i) {
        REQUIRE(std::isfinite(L[static_cast<std::size_t>(i)]));
        REQUIRE(std::isfinite(R[static_cast<std::size_t>(i)]));
    }

    constexpr int kFrames = 4800;
    const auto orig = sine(kFrames, 1000.0f, 0.25f);
    auto S = orig;
    auto SR = orig;
    p.process(S.data(), SR.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(std::isfinite(S[static_cast<std::size_t>(i)]));
        REQUIRE(std::isfinite(SR[static_cast<std::size_t>(i)]));
    }
    // Audio actually flows again instead of staying dead-silent.
    REQUIRE(rmsGain(orig, S, static_cast<std::size_t>(kFrames) / 2) > 0.01);
}
