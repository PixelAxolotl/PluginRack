#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "pluginrack/Meters.h"
#include "pluginrack/MixerEngine.h"

namespace {

pluginrack::Scene makeScene() {
    pluginrack::Scene scene;
    scene.schemaVersion = 1;
    scene.name = "meters";
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

} // namespace

TEST_CASE("peak holds through one block") {
    pluginrack::MeterBank meters;
    meters.observeChannel(1, 0.5f);
    REQUIRE(meters.channelPeak(1) == 0.5f);
}

TEST_CASE("peak decays 20 dB per second") {
    pluginrack::MeterBank meters;
    meters.observeChannel(1, 1.0f);
    meters.advance(48000, 48000.0);
    REQUIRE(meters.channelPeak(1) == Catch::Approx(0.1f).epsilon(0.001));
}

TEST_CASE("engine reports post-pan channel peak") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);
    pluginrack::MixerEngine engine;
    std::string error;
    REQUIRE(engine.prepare(scene, 256, &error));

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    REQUIRE(engine.meters().channelPeak(1) == Catch::Approx(0.35355339f));
}

TEST_CASE("engine reports output peak after send gain") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1, -6.0f);
    pluginrack::MixerEngine engine;
    std::string error;
    REQUIRE(engine.prepare(scene, 256, &error));

    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    REQUIRE(engine.meters().outputPeak(1) == Catch::Approx(0.35355339f * 0.5011872f));
}

TEST_CASE("unknown id reports zero") {
    pluginrack::MeterBank meters;
    REQUIRE(meters.channelPeak(999) == 0.0f);
    REQUIRE(meters.outputPeak(999) == 0.0f);
}

TEST_CASE("realtime meter updates do not register unknown ids") {
    pluginrack::MeterBank meters;
    meters.observeChannelRealtime(999, 1.0f);
    REQUIRE(meters.channelPeak(999) == 0.0f);

    meters.observeChannel(1, 0.0f);
    meters.observeChannelRealtime(1, 0.5f);
    REQUIRE(meters.channelPeak(1) == 0.5f);
}

TEST_CASE("setTargets replaces the metered id set") {
    pluginrack::MeterBank meters;
    const std::vector<std::uint32_t> channels{1, 2};
    const std::vector<std::uint32_t> outputs{3};
    meters.setTargets(channels.data(), channels.size(), outputs.data(), outputs.size());

    meters.observeChannelRealtime(1, 0.5f);
    meters.observeOutputRealtime(3, 0.25f);
    REQUIRE(meters.channelPeak(1) == 0.5f);
    REQUIRE(meters.outputPeak(3) == 0.25f);

    const std::vector<std::uint32_t> nextChannels{7};
    const std::vector<std::uint32_t> noOutputs;
    meters.setTargets(nextChannels.data(), nextChannels.size(),
                      noOutputs.data(), noOutputs.size());

    // The old ids are gone, so realtime updates for them are ignored.
    meters.observeChannelRealtime(1, 0.5f);
    meters.observeOutputRealtime(3, 0.25f);
    REQUIRE(meters.channelPeak(1) == 0.0f);
    REQUIRE(meters.outputPeak(3) == 0.0f);

    meters.observeChannelRealtime(7, 0.25f);
    REQUIRE(meters.channelPeak(7) == 0.25f);
}

TEST_CASE("retargeting while the audio thread updates stays memory safe") {
    pluginrack::MeterBank meters;
    std::atomic<bool> stop{false};
    const std::vector<std::uint32_t> first{1, 2, 3, 4};
    const std::vector<std::uint32_t> second{5, 6, 7, 8};
    meters.setTargets(first.data(), first.size(), first.data(), first.size());

    std::thread audio([&meters, &stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            for (std::uint32_t id = 1; id <= 8; ++id) {
                meters.observeChannelRealtime(id, 0.5f);
                meters.observeOutputRealtime(id, 0.5f);
                (void) meters.channelPeak(id);
            }
            meters.advance(64, 48000.0);
        }
    });

    for (int i = 0; i < 2000; ++i) {
        const auto& ids = (i % 2 == 0) ? first : second;
        meters.setTargets(ids.data(), ids.size(), ids.data(), ids.size());
    }

    stop.store(true, std::memory_order_relaxed);
    audio.join();

    // The churn left usable storage behind: a fresh registration still reports.
    const std::vector<std::uint32_t> after{42};
    meters.setTargets(after.data(), after.size(), nullptr, 0);
    meters.observeChannelRealtime(42, 0.75f);
    REQUIRE(meters.channelPeak(42) == 0.75f);
    REQUIRE(meters.channelPeak(1) == 0.0f);
}
