#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "pluginrack/MixerEngine.h"

#include "support/AllocCounter.h"

namespace {

constexpr float kCenterPanGain = 0.70710678f;

pluginrack::Scene makeScene() {
    pluginrack::Scene scene;
    scene.schemaVersion = 1;
    scene.name = "mixer";
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;
    return scene;
}

void addChannel(pluginrack::Scene& scene, std::uint32_t id, float gainDb = 0.0f,
                float pan = 0.0f) {
    pluginrack::Channel channel;
    channel.id = id;
    channel.name = "ch" + std::to_string(id);
    channel.layout = pluginrack::ChannelLayout::Mono;
    channel.gainDb = gainDb;
    channel.pan = pan;
    scene.channels.push_back(channel);
}

void addOutput(pluginrack::Scene& scene, std::uint32_t id) {
    pluginrack::Output output;
    output.id = id;
    output.name = "out" + std::to_string(id);
    scene.outputs.push_back(output);
}

void addRoute(pluginrack::Scene& scene, std::uint32_t channelId, std::uint32_t outputId,
              float sendGainDb = 0.0f) {
    pluginrack::Route route;
    route.channelId = channelId;
    route.outputId = outputId;
    route.sendGainDb = sendGainDb;
    scene.routes.push_back(route);
}

struct StereoBuffer {
    std::vector<float> left;
    std::vector<float> right;
    float* ptrs[2] = {nullptr, nullptr};

    explicit StereoBuffer(int frames, float value = 0.0f)
        : left(static_cast<std::size_t>(frames), value),
          right(static_cast<std::size_t>(frames), value) {
        ptrs[0] = left.data();
        ptrs[1] = right.data();
    }

    pluginrack::AudioBlock inputBlock() const {
        return pluginrack::AudioBlock{const_cast<float* const*>(ptrs), 2,
                                      static_cast<int>(left.size())};
    }

    pluginrack::AudioBlock outputBlock() {
        return pluginrack::AudioBlock{ptrs, 2, static_cast<int>(left.size())};
    }
};

pluginrack::MixerEngine prepareEngine(const pluginrack::Scene& scene, int maxBlockFrames = 256) {
    pluginrack::MixerEngine engine;
    std::string error;
    REQUIRE(engine.prepare(scene, maxBlockFrames, &error));
    return engine;
}

} // namespace

TEST_CASE("unity route passes signal at center pan") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == Catch::Approx(0.35355339f));
        REQUIRE(out.right[static_cast<std::size_t>(i)] == Catch::Approx(0.35355339f));
    }
}

TEST_CASE("send gain of -6 dB scales the route") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1, -6.0f);
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    const float expected = 0.5f * kCenterPanGain * 0.5011872f;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == Catch::Approx(expected));
        REQUIRE(out.right[static_cast<std::size_t>(i)] == Catch::Approx(expected));
    }
}

TEST_CASE("split send levels are applied per side") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1, 0.0f);
    scene.routes.front().sendGainDbR = -60.0f;
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    // The live engine applies sendGainDbL to the left and sendGainDbR to the
    // right; a single cached gain would have sent both at unity. -60 dB is the
    // engine's silence floor, so the right side must be exactly silent.
    const float leftExpected = 0.5f * kCenterPanGain;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == Catch::Approx(leftExpected));
        REQUIRE(out.right[static_cast<std::size_t>(i)] == 0.0f);
    }
}

TEST_CASE("channel trim adds to the fader gain") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    scene.channels.front().trimDb = 6.0f;
    addOutput(scene, 1);
    addRoute(scene, 1, 1);
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.25f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    // +6 dB trim on top of the constant-power center gain.
    const float expected = 0.25f * 1.9952623f * kCenterPanGain;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == Catch::Approx(expected));
        REQUIRE(out.right[static_cast<std::size_t>(i)] == Catch::Approx(expected));
    }
}

TEST_CASE("mixer gate attenuates channel audio below threshold") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);

    auto& gate = scene.channels.front().effects[1];
    gate.type = pluginrack::DynamicsEffectType::Gate;
    gate.bypassed = false;
    gate.thresholdDb = -11.8f;
    gate.ratio = 55.8f;
    gate.attackMs = 1.0f;
    gate.releaseMs = 10.0f;

    pluginrack::MixerEngine engine = prepareEngine(scene, 48000);
    constexpr int kFrames = 48000;
    StereoBuffer in(kFrames, 0.0630957f); // -24 dBFS, below the threshold
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    REQUIRE(std::max(std::fabs(out.left.back()), std::fabs(out.right.back())) <
            0.0001f);
}

TEST_CASE("output fader gain scales the summed bus") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    scene.outputs.front().gainDb = -6.0f;
    addRoute(scene, 1, 1);
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    const float expected = 0.5f * kCenterPanGain * 0.5011872f;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == Catch::Approx(expected));
        REQUIRE(out.right[static_cast<std::size_t>(i)] == Catch::Approx(expected));
    }
}

TEST_CASE("muted output is silent") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    scene.outputs.front().muted = true;
    addRoute(scene, 1, 1);
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(out.right[static_cast<std::size_t>(i)] == 0.0f);
    }
}

TEST_CASE("one channel reaches two outputs independently") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addOutput(scene, 2);
    addRoute(scene, 1, 1, 0.0f);
    addRoute(scene, 1, 2, -12.0f);
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out1(kFrames, 0.0f);
    StereoBuffer out2(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out1.outputBlock(), out2.outputBlock()};
    engine.render(inputs, outputs);

    const float expected1 = 0.35355339f;
    const float expected2 = 0.35355339f * 0.2511886f;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out1.left[static_cast<std::size_t>(i)] == Catch::Approx(expected1));
        REQUIRE(out2.left[static_cast<std::size_t>(i)] == Catch::Approx(expected2));
    }
}

TEST_CASE("muted channel is silent") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);
    scene.channels[0].muted = true;
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(out.right[static_cast<std::size_t>(i)] == 0.0f);
    }
}

TEST_CASE("solo isolates the soloed channel") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addChannel(scene, 2);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);
    addRoute(scene, 2, 1);
    scene.channels[1].soloed = true;
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in1(kFrames, 0.5f);
    StereoBuffer in2(kFrames, 0.25f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in1.inputBlock(), in2.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    const float expected = 0.25f * kCenterPanGain;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == Catch::Approx(expected));
        REQUIRE(out.right[static_cast<std::size_t>(i)] == Catch::Approx(expected));
    }
}

TEST_CASE("multiple routes into one output sum") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addChannel(scene, 2);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);
    addRoute(scene, 2, 1);
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in1(kFrames, 0.5f);
    StereoBuffer in2(kFrames, 0.25f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in1.inputBlock(), in2.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    const float expected = 0.75f * kCenterPanGain;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == Catch::Approx(expected));
        REQUIRE(out.right[static_cast<std::size_t>(i)] == Catch::Approx(expected));
    }
}

TEST_CASE("hard pan reaches the constant-power endpoints") {
    pluginrack::Scene scene = makeScene();
    float expectedLeft = 0.0f;
    float expectedRight = 0.0f;

    SECTION("left") {
        addChannel(scene, 1, 0.0f, -1.0f);
        expectedLeft = 0.5f;
    }
    SECTION("right") {
        addChannel(scene, 1, 0.0f, 1.0f);
        expectedRight = 0.5f;
    }
    addOutput(scene, 1);
    addRoute(scene, 1, 1);
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] ==
                Catch::Approx(expectedLeft).margin(0.000001f));
        REQUIRE(out.right[static_cast<std::size_t>(i)] ==
                Catch::Approx(expectedRight).margin(0.000001f));
    }
}

TEST_CASE("mute wins over solo") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);
    scene.channels[0].muted = true;
    scene.channels[0].soloed = true;
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(out.right[static_cast<std::size_t>(i)] == 0.0f);
    }
}

TEST_CASE("gain of -60 dB is exact silence") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1, -60.0f);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 1.0f);
    StereoBuffer out(kFrames, 0.0f);
    // Poison the outputs so a missing write cannot pass.
    std::memset(out.left.data(), 0xFF, out.left.size() * sizeof(float));
    std::memset(out.right.data(), 0xFF, out.right.size() * sizeof(float));
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(out.right[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(std::signbit(out.left[static_cast<std::size_t>(i)]) == false);
        REQUIRE(std::signbit(out.right[static_cast<std::size_t>(i)]) == false);
    }
}

TEST_CASE("prepare rejects a scene with no outputs") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);

    pluginrack::MixerEngine engine;
    std::string error;
    REQUIRE(engine.prepare(scene, 256, &error) == false);
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("render allocates nothing") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);

    AllocCounter::reset();
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    // Only the render call itself must be allocation-free; everything
    // above (including these scratch buffers) is set up beforehand.
    AllocCounter::reset();
    engine.render(inputs, outputs);

    REQUIRE(AllocCounter::allocations() == 0);
}

namespace {

// Renders a 1 kHz sine (0.5 amplitude, mono upmixed to L=R) through the
// engine in 256-frame blocks and returns the peak of the settled output
// left bus. Phase advances continuously across blocks.
float sineOutputPeak(pluginrack::MixerEngine& engine) {
    constexpr int kFrames = 2048;
    constexpr int kBlock = 256;
    constexpr int kSettled = 512;
    const double step = 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
    std::vector<float> sine(static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i) {
        sine[static_cast<std::size_t>(i)] =
            static_cast<float>(0.5 * std::sin(step * static_cast<double>(i)));
    }
    std::vector<float> outLeft(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> outRight(static_cast<std::size_t>(kFrames), 0.0f);
    for (int offset = 0; offset < kFrames; offset += kBlock) {
        float* inPtrs[2] = {sine.data() + offset, sine.data() + offset};
        float* outPtrs[2] = {outLeft.data() + offset, outRight.data() + offset};
        const pluginrack::AudioBlock inputs[] = {pluginrack::AudioBlock{inPtrs, 2, kBlock}};
        pluginrack::AudioBlock outputs[] = {pluginrack::AudioBlock{outPtrs, 2, kBlock}};
        engine.render(inputs, outputs);
    }
    float peak = 0.0f;
    for (int i = kFrames - kSettled; i < kFrames; ++i) {
        const float magnitude = std::fabs(outLeft[static_cast<std::size_t>(i)]);
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return peak;
}

pluginrack::Scene eqTestScene() {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);
    return scene;
}

} // namespace

TEST_CASE("mixer +6dB peak at 1k boosts 1k sine approx 2x") {
    pluginrack::Scene scene = eqTestScene();
    pluginrack::MixerEngine reference = prepareEngine(scene);
    const float referencePeak = sineOutputPeak(reference);
    REQUIRE(referencePeak > 0.1f);

    scene.channels[0].eqEnabled = true;
    scene.channels[0].eqBands = pluginrack::defaultEqBands();
    scene.channels[0].eqBands[3].gainDb = 6.0f; // the 1 kHz peak band
    pluginrack::MixerEngine boosted = prepareEngine(scene);
    const float boostedPeak = sineOutputPeak(boosted);

    REQUIRE(boostedPeak / referencePeak == Catch::Approx(2.0f).margin(0.08f));
}

TEST_CASE("mixer setChannelEq applies bands live and ignores bad indexes") {
    pluginrack::Scene scene = eqTestScene();
    scene.channels[0].eqEnabled = true;
    pluginrack::MixerEngine reference = prepareEngine(scene);

    auto bands = pluginrack::defaultEqBands();
    bands[3].gainDb = 6.0f;
    pluginrack::MixerEngine engine = prepareEngine(scene);
    engine.setChannelEq(99, bands);
    engine.setChannelEq(-1, bands);
    engine.setChannelEq(0, bands);

    REQUIRE(sineOutputPeak(engine) / sineOutputPeak(reference) ==
            Catch::Approx(2.0f).margin(0.08f));
}

TEST_CASE("mixer setOutputEq boosts the summed bus") {
    pluginrack::Scene scene = eqTestScene();
    scene.outputs[0].eqEnabled = true;
    pluginrack::MixerEngine reference = prepareEngine(scene);

    auto bands = pluginrack::defaultEqBands();
    bands[3].gainDb = 6.0f;
    pluginrack::MixerEngine engine = prepareEngine(scene);
    engine.setOutputEq(99, bands);
    engine.setOutputEq(-1, bands);
    engine.setOutputEq(0, bands);

    REQUIRE(sineOutputPeak(engine) / sineOutputPeak(reference) ==
            Catch::Approx(2.0f).margin(0.08f));
}

TEST_CASE("mixer render with active eq allocates nothing") {
    pluginrack::Scene scene = eqTestScene();
    scene.channels[0].eqEnabled = true;
    scene.channels[0].eqBands = pluginrack::defaultEqBands();
    scene.channels[0].eqBands[3].gainDb = 6.0f;
    scene.outputs[0].eqEnabled = true;

    AllocCounter::reset();
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    AllocCounter::reset();
    engine.render(inputs, outputs);

    REQUIRE(AllocCounter::allocations() == 0);
}

TEST_CASE("mixer scene eq delay shifts the channel output") {
    pluginrack::Scene scene = eqTestScene();
    scene.channels[0].eqEnabled = true;
    for (auto& band : scene.channels[0].eqBands) {
        band.enabled = false; // isolate the delay
    }
    scene.channels[0].eqDelayMs = 1.0f; // 48 samples at 48 kHz
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.0f);
    in.left[0] = 1.0f;
    in.right[4] = 0.5f; // offset tap proves the two delay lines stay independent
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    for (int i = 0; i < 48; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == 0.0f);
    }
    REQUIRE(out.left[48] == Catch::Approx(kCenterPanGain));

    for (int i = 0; i < 52; ++i) {
        REQUIRE(out.right[static_cast<std::size_t>(i)] == 0.0f);
    }
    REQUIRE(out.right[52] == Catch::Approx(0.5f * kCenterPanGain));
}

TEST_CASE("mixer render with active eq delay allocates nothing") {
    pluginrack::Scene scene = eqTestScene();
    scene.channels[0].eqEnabled = true;
    scene.channels[0].eqDelayMs = 500.0f;
    scene.outputs[0].eqEnabled = true;
    scene.outputs[0].eqDelayMs = 100.0f;

    AllocCounter::reset();
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    AllocCounter::reset();
    engine.render(inputs, outputs);

    REQUIRE(AllocCounter::allocations() == 0);
}

TEST_CASE("mixer non-finite input frames are flushed at the eq tap") {
    pluginrack::Scene scene = eqTestScene();
    scene.channels[0].eqEnabled = true;
    pluginrack::MixerEngine engine = prepareEngine(scene);

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    in.left[64] = nan;
    in.right[64] = nan;
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(std::isfinite(out.left[static_cast<std::size_t>(i)]));
        REQUIRE(std::isfinite(out.right[static_cast<std::size_t>(i)]));
    }
}

TEST_CASE("mixer setChannelEqBypassed gates the eq live without re-prepare") {
    pluginrack::Scene off = eqTestScene();
    pluginrack::MixerEngine reference = prepareEngine(off);
    const float refPeak = sineOutputPeak(reference);
    REQUIRE(refPeak > 0.1f);

    pluginrack::Scene scene = eqTestScene();
    scene.channels[0].eqEnabled = true;
    scene.channels[0].eqBands = pluginrack::defaultEqBands();
    scene.channels[0].eqBands[3].gainDb = 6.0f;
    pluginrack::MixerEngine engine = prepareEngine(scene);

    engine.setChannelEqBypassed(99, true); // ignored, must not crash

    REQUIRE(sineOutputPeak(engine) / refPeak == Catch::Approx(2.0f).margin(0.08f));
    engine.setChannelEqBypassed(0, true);
    REQUIRE(sineOutputPeak(engine) / refPeak == Catch::Approx(1.0f).margin(0.01f));
    engine.setChannelEqBypassed(0, false);
    REQUIRE(sineOutputPeak(engine) / refPeak == Catch::Approx(2.0f).margin(0.08f));
}

TEST_CASE("mixer setOutputEqBypassed gates the output eq") {
    pluginrack::Scene off = eqTestScene();
    pluginrack::MixerEngine reference = prepareEngine(off);
    const float refPeak = sineOutputPeak(reference);
    REQUIRE(refPeak > 0.1f);

    pluginrack::Scene scene = eqTestScene();
    scene.outputs[0].eqEnabled = true;
    scene.outputs[0].eqBands = pluginrack::defaultEqBands();
    scene.outputs[0].eqBands[3].gainDb = 6.0f;
    pluginrack::MixerEngine engine = prepareEngine(scene);

    engine.setOutputEqBypassed(-1, true); // ignored, must not crash

    REQUIRE(sineOutputPeak(engine) / refPeak == Catch::Approx(2.0f).margin(0.08f));
    engine.setOutputEqBypassed(0, true);
    REQUIRE(sineOutputPeak(engine) / refPeak == Catch::Approx(1.0f).margin(0.01f));
    engine.setOutputEqBypassed(0, false);
    REQUIRE(sineOutputPeak(engine) / refPeak == Catch::Approx(2.0f).margin(0.08f));
}
