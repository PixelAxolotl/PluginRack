#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "pluginrack/DeviceRegistry.h"
#include "pluginrack/MixerEngine.h"
#include "pluginrack/SceneAvailability.h"

namespace {

using Catch::Matchers::ContainsSubstring;

class FakeRegistry : public pluginrack::DeviceRegistry {
public:
    std::unordered_set<std::string> inputs;
    std::unordered_set<std::string> outputs;

    bool hasInput(const std::string& deviceId) const override {
        return inputs.contains(deviceId);
    }

    bool hasOutput(const std::string& deviceId) const override {
        return outputs.contains(deviceId);
    }
};

pluginrack::Scene makeScene() {
    pluginrack::Scene scene;
    scene.schemaVersion = 1;
    scene.name = "availability";
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;
    return scene;
}

void addChannel(pluginrack::Scene& scene, std::uint32_t id, const std::string& deviceId) {
    pluginrack::Channel channel;
    channel.id = id;
    channel.name = "ch" + std::to_string(id);
    channel.layout = pluginrack::ChannelLayout::Mono;
    channel.sourceDeviceId = deviceId;
    scene.channels.push_back(channel);
}

void addOutput(pluginrack::Scene& scene, std::uint32_t id, const std::string& deviceId) {
    pluginrack::Output output;
    output.id = id;
    output.name = "out" + std::to_string(id);
    output.deviceId = deviceId;
    scene.outputs.push_back(output);
}

void addRoute(pluginrack::Scene& scene, std::uint32_t channelId, std::uint32_t outputId) {
    pluginrack::Route route;
    route.channelId = channelId;
    route.outputId = outputId;
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

TEST_CASE("missing input stops only that channel") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1, "mic-ok");
    addChannel(scene, 2, "mic-gone");
    addOutput(scene, 1, "mon");
    addRoute(scene, 1, 1);
    addRoute(scene, 2, 1);

    FakeRegistry registry;
    registry.inputs.insert("mic-ok");
    registry.outputs.insert("mon");

    const pluginrack::SceneAvailability avail =
        pluginrack::resolveAvailability(scene, registry);
    REQUIRE(avail.stoppedChannels == std::vector<std::uint32_t>{1});
    REQUIRE(avail.warnings.size() == 1);
    REQUIRE_THAT(avail.warnings[0], ContainsSubstring("mic-gone"));

    // The surviving channel still renders through the same output.
    pluginrack::MixerEngine engine;
    std::string error;
    REQUIRE(engine.prepare(scene, avail, 256, &error));
    constexpr int kFrames = 128;
    StereoBuffer in1(kFrames, 0.5f);
    StereoBuffer in2(kFrames, 0.5f);
    StereoBuffer out(kFrames, 0.0f);
    const pluginrack::AudioBlock inputs[] = {in1.inputBlock(), in2.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out.outputBlock()};
    engine.render(inputs, outputs);

    constexpr float kExpected = 0.5f * 0.70710678f;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out.left[static_cast<std::size_t>(i)] == Catch::Approx(kExpected));
        REQUIRE(out.right[static_cast<std::size_t>(i)] == Catch::Approx(kExpected));
    }
}

TEST_CASE("missing output is disabled and rest of scene renders") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1, "mic-ok");
    addOutput(scene, 1, "mon");
    addOutput(scene, 2, "virt-1");
    addRoute(scene, 1, 1);
    addRoute(scene, 1, 2);

    FakeRegistry registry;
    registry.inputs.insert("mic-ok");
    registry.outputs.insert("mon");

    const pluginrack::SceneAvailability avail =
        pluginrack::resolveAvailability(scene, registry);
    REQUIRE(avail.disabledOutputs == std::vector<std::uint32_t>{1});
    REQUIRE(avail.warnings.size() == 1);
    REQUIRE_THAT(avail.warnings[0], ContainsSubstring("virt-1"));

    pluginrack::MixerEngine engine;
    std::string error;
    REQUIRE(engine.prepare(scene, avail, 256, &error));
    constexpr int kFrames = 128;
    StereoBuffer in(kFrames, 0.5f);
    StereoBuffer out1(kFrames, 0.0f);
    StereoBuffer out2(kFrames, 0.0f);
    // Poison the outputs so a missing write cannot pass.
    std::memset(out1.left.data(), 0xFF, out1.left.size() * sizeof(float));
    std::memset(out1.right.data(), 0xFF, out1.right.size() * sizeof(float));
    std::memset(out2.left.data(), 0xFF, out2.left.size() * sizeof(float));
    std::memset(out2.right.data(), 0xFF, out2.right.size() * sizeof(float));
    const pluginrack::AudioBlock inputs[] = {in.inputBlock()};
    pluginrack::AudioBlock outputs[] = {out1.outputBlock(), out2.outputBlock()};
    engine.render(inputs, outputs);

    constexpr float kExpected = 0.5f * 0.70710678f;
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out1.left[static_cast<std::size_t>(i)] == Catch::Approx(kExpected));
        REQUIRE(out1.right[static_cast<std::size_t>(i)] == Catch::Approx(kExpected));
        REQUIRE(out2.left[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(out2.right[static_cast<std::size_t>(i)] == 0.0f);
    }
}

TEST_CASE("shared missing input stops every channel but warns once") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1, "mic-gone");
    addChannel(scene, 2, "mic-gone");
    addOutput(scene, 1, "mon");
    addRoute(scene, 1, 1);
    addRoute(scene, 2, 1);

    FakeRegistry registry;
    registry.outputs.insert("mon");

    const pluginrack::SceneAvailability avail =
        pluginrack::resolveAvailability(scene, registry);
    REQUIRE(avail.stoppedChannels == std::vector<std::uint32_t>{0, 1});
    REQUIRE(avail.warnings.size() == 1);
    REQUIRE_THAT(avail.warnings[0], ContainsSubstring("mic-gone"));
}

TEST_CASE("all devices present yields no warnings") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1, "mic-ok");
    addOutput(scene, 1, "mon");
    addRoute(scene, 1, 1);

    FakeRegistry registry;
    registry.inputs.insert("mic-ok");
    registry.outputs.insert("mon");

    const pluginrack::SceneAvailability avail =
        pluginrack::resolveAvailability(scene, registry);
    REQUIRE(avail.warnings.empty());
    REQUIRE(avail.stoppedChannels.empty());
    REQUIRE(avail.disabledOutputs.empty());
}
