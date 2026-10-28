#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
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
