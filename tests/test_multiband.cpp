#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "pluginrack/MultibandEffect.h"
#include "pluginrack/SceneCodec.h"
#include "pluginrack/TraceData.h"

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

// One band's output as the render path produces it: solo leaves exactly that
// band in the mix, so this is the split itself, not a coefficient estimate.
std::vector<float> renderSoloedBand(const MultibandEffect& effect, int soloBand,
                                    const std::vector<float>& input) {
    auto solo = effect;
    for (std::size_t i = 0; i < solo.bands.size(); ++i)
        solo.bands[i].soloed = static_cast<int>(i) == soloBand;
    MultibandProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(solo);
    auto left = input;
    auto right = input;
    processor.process(left.data(), right.data(), static_cast<int>(left.size()));
    return left;
}

double gainDbOf(const std::vector<float>& in, const std::vector<float>& out, std::size_t begin) {
    const double gain = rmsGain(in, out, begin);
    return gain > 0.0 ? 20.0 * std::log10(gain) : -200.0;
}

// The multiband effect with every band's dynamics neutral, so a probe measures
// the crossover alone.
MultibandEffect flatMultibandEffect(std::size_t bands, std::vector<float> crossovers) {
    auto effect = pluginrack::makeDefaultMultibandEffect();
    effect.bands.resize(bands);
    effect.crossovers = std::move(crossovers);
    for (auto& band : effect.bands) {
        band.ratio = 1.0f;
        band.thresholdDb = 0.0f;
        band.makeupDb = 0.0f;
        band.inputGainDb = 0.0f;
    }
    return effect;
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
    // effect set afterwards with more crossovers must still process without
    // overrunning them.
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

TEST_CASE("multiband LR4 bands sum flat across every crossover") {
    // The strong test: solo each band, sum the soloed outputs sample by sample,
    // and compare with the input. This walks the render path, not the
    // coefficients, and covers every split including the widened six-band
    // layout. 0.5 dB is the pass.
    const auto effect = flatMultibandEffect(6, {200.0f, 800.0f, 3000.0f, 8000.0f, 12000.0f});
    const int bands = static_cast<int>(effect.bands.size());
    constexpr int kFrames = 65536;
    const std::size_t begin = static_cast<std::size_t>(kFrames) / 2;

    auto reference = effect;
    MultibandProcessor full;
    full.prepare(48000.0);
    full.setEffect(reference);

    double worstDb = 0.0;
    for (const float freq : {50.0f, 200.0f, 400.0f, 800.0f, 1600.0f, 3000.0f,
                             5000.0f, 8000.0f, 10000.0f, 12000.0f, 16000.0f}) {
        const auto input = sine(kFrames, freq, 0.5f);
        std::vector<float> sum(static_cast<std::size_t>(kFrames), 0.0f);
        for (int band = 0; band < bands; ++band) {
            const auto bandOutput = renderSoloedBand(effect, band, input);
            for (int i = 0; i < kFrames; ++i)
                sum[static_cast<std::size_t>(i)] += bandOutput[static_cast<std::size_t>(i)];
        }
        const double summedDb = gainDbOf(input, sum, begin);
        worstDb = std::max(worstDb, std::abs(summedDb));
        CHECK(summedDb > -0.5);
        CHECK(summedDb < 0.5);

        // The soloed bands are the render path: their sum must equal the
        // unsoloed output, so the drawn curves describe the same signal.
        auto left = input;
        auto right = input;
        full.reset();
        full.process(left.data(), right.data(), kFrames);
        for (std::size_t i = begin; i < sum.size(); ++i)
            CHECK(std::abs(sum[i] - left[i]) < 1.0e-4f);
    }
    CHECK(worstDb < 0.5);
}

TEST_CASE("multiband LR4 split points meet at -6 dB with no crossover bump") {
    // A real LR4 pair is -6 dB on both sides at the split. The old Butterworth
    // high-pass difference put the low band +1.76 dB above unity there.
    const auto effect = flatMultibandEffect(4, {250.0f, 2000.0f, 6000.0f});
    constexpr int kFrames = 32768;
    const std::size_t begin = static_cast<std::size_t>(kFrames) / 2;

    for (std::size_t split = 0; split < effect.crossovers.size(); ++split) {
        const float crossover = effect.crossovers[split];
        const auto input = sine(kFrames, crossover, 0.5f);
        for (const int band : {static_cast<int>(split), static_cast<int>(split) + 1}) {
            const auto output = renderSoloedBand(effect, band, input);
            const double gainDb = gainDbOf(input, output, begin);
            INFO("band " << band << " at " << crossover << " Hz reads " << gainDb << " dB");
            CHECK(gainDb > -6.6);
            CHECK(gainDb < -5.4);
        }
    }

    // No band may exceed unity anywhere in its own pass band either.
    for (const float freq : {60.0f, 100.0f, 500.0f, 1000.0f, 4000.0f, 9000.0f}) {
        const auto input = sine(kFrames, freq, 0.5f);
        for (int band = 0; band < static_cast<int>(effect.bands.size()); ++band) {
            const auto output = renderSoloedBand(effect, band, input);
            CHECK(gainDbOf(input, output, begin) < 0.5);
        }
    }
}

TEST_CASE("drawn multiband curve matches the measured band output") {
    // The target trace is the LR4 split's real magnitude, so the drawn peak of
    // a band has to equal the gain that band's output actually delivers.
    const auto effect = flatMultibandEffect(4, {250.0f, 2000.0f, 6000.0f});
    constexpr std::size_t kBins = 1024;
    const auto trace = pluginrack::makeMultibandTargetTrace(effect, kBins);
    REQUIRE(trace.size() == kBins);

    const auto frequencyAt = [](std::size_t bin) {
        return 20.0f * std::pow(1000.0f, static_cast<float>(bin)
                                             / static_cast<float>(kBins - 1));
    };

    // Band 1 sits between the first two splits; its plateau is the peak.
    std::size_t peakBin = 0;
    float peakDb = -1.0e9f;
    for (std::size_t bin = 0; bin < kBins; ++bin) {
        const float hz = frequencyAt(bin);
        if (hz <= effect.crossovers[0] || hz >= effect.crossovers[1]) continue;
        if (trace[bin] > peakDb) { peakDb = trace[bin]; peakBin = bin; }
    }
    const float probeHz = frequencyAt(peakBin);
    constexpr int kFrames = 32768;
    const auto input = sine(kFrames, probeHz, 0.5f);
    const auto output = renderSoloedBand(effect, 1, input);
    const double measuredDb = gainDbOf(input, output, static_cast<std::size_t>(kFrames) / 2);
    CHECK(std::abs(static_cast<double>(peakDb) - measuredDb) < 1.0);

    // A trim on the band moves the drawn curve by exactly that many dB.
    auto trimmed = effect;
    trimmed.bands[1].makeupDb = 3.0f;
    const auto trimmedTrace = pluginrack::makeMultibandTargetTrace(trimmed, kBins);
    CHECK(trimmedTrace[peakBin] == Catch::Approx(peakDb + 3.0f).margin(0.01f));
}
