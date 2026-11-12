#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "pluginrack/InsertChain.h"
#include "pluginrack/MixerEngine.h"
#include "pluginrack/SampleScrub.h"

#include "support/TestPlugins.h"

namespace {

using pluginrack::AudioBlock;
using pluginrack::InsertSlot;

class PoisonPlugin : public pluginrack::IPluginInstance {
public:
    explicit PoisonPlugin(float value) : value_(value) {}
    void prepare(double /*sampleRate*/, int /*maxBlockFrames*/) override {}
    pluginrack::ProcessResult process(AudioBlock& block) noexcept override {
        if (block.channels == nullptr) return pluginrack::ProcessResult::Ok;
        for (int c = 0; c < block.numChannels; ++c) {
            float* channel = block.channels[c];
            if (channel == nullptr) continue;
            for (int i = 0; i < block.numFrames; ++i) channel[i] = value_;
        }
        return pluginrack::ProcessResult::Ok;
    }
    void reset() noexcept override {}

private:
    float value_;
};

class PoisonFactory : public pluginrack::IPluginFactory {
public:
    std::unique_ptr<pluginrack::IPluginInstance> create(
        const std::string& pluginPath, std::string* error) override {
        if (error != nullptr) error->clear();
        if (pluginPath == "/inf") {
            return std::make_unique<PoisonPlugin>(std::numeric_limits<float>::infinity());
        }
        if (pluginPath == "/nan") {
            return std::make_unique<PoisonPlugin>(std::numeric_limits<float>::quiet_NaN());
        }
        return std::make_unique<pluginrack::test::HalvePlugin>();
    }
};

InsertSlot makeSlot(std::uint32_t id, const std::string& path) {
    InsertSlot slot;
    slot.id = id;
    slot.pluginPath = path;
    slot.pluginName = "poison";
    return slot;
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

    AudioBlock block() { return AudioBlock{ptrs, 2, static_cast<int>(left.size())}; }
};

std::string readSource(const char* relativePath) {
    const std::string path = std::string(PLUGINRACK_SOURCE_DIR) + "/" + relativePath;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

} // namespace

TEST_CASE("NaN from an insert is zeroed before the next insert",
          "[sample][scrub]") {
    PoisonFactory factory;
    const std::vector<InsertSlot> slots = {makeSlot(1, "/nan"), makeSlot(2, "/halve")};
    pluginrack::InsertChain chain(slots, factory, 48000.0, 256, /*scrubEnabled=*/true);

    StereoBuffer buffer(16, 1.0f);
    AudioBlock block = buffer.block();
    chain.process(block);

    // 16 frames x 2 channels were poison.
    CHECK(chain.scrubbedSamples() == 32);
    for (float sample : buffer.left) CHECK(std::isfinite(sample));
    for (float sample : buffer.right) CHECK(std::isfinite(sample));
    for (float sample : buffer.left) CHECK(sample == 0.0f);
}

TEST_CASE("the scrub counter is exact across repeated blocks", "[sample][scrub]") {
    PoisonFactory factory;
    const std::vector<InsertSlot> slots = {makeSlot(1, "/inf")};
    pluginrack::InsertChain chain(slots, factory, 48000.0, 256, /*scrubEnabled=*/true);

    StereoBuffer buffer(8, 1.0f);
    AudioBlock block = buffer.block();
    for (int i = 0; i < 3; ++i) chain.process(block);

    // 3 blocks x 8 frames x 2 channels.
    CHECK(chain.scrubbedSamples() == 48);
    for (float sample : buffer.left) CHECK(sample == 0.0f);
}

TEST_CASE("the mixer engine scrubs insert output before the mix bus",
          "[sample][scrub]") {
    PoisonFactory factory;
    pluginrack::Scene scene;
    scene.schemaVersion = 1;
    scene.name = "poison";
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;

    pluginrack::Channel channel;
    channel.id = 1;
    channel.name = "ch1";
    channel.layout = pluginrack::ChannelLayout::Mono;
    channel.inserts.push_back(makeSlot(1, "/nan"));
    scene.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    output.name = "out1";
    scene.outputs.push_back(output);

    pluginrack::Route route;
    route.channelId = 1;
    route.outputId = 1;
    scene.routes.push_back(route);

    pluginrack::MixerEngine engine;
    engine.setPluginFactory(&factory);
    engine.setScrubEnabled(true);
    std::string error;
    REQUIRE(engine.prepare(scene, 256, &error));

    constexpr int kFrames = 32;
    std::vector<float> mono(static_cast<std::size_t>(kFrames), 1.0f);
    float* inPtrs[1] = {mono.data()};
    const AudioBlock inputs[] = {AudioBlock{inPtrs, 1, kFrames}};

    std::vector<float> outLeft(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> outRight(static_cast<std::size_t>(kFrames), 0.0f);
    float* outPtrs[2] = {outLeft.data(), outRight.data()};
    AudioBlock outputs[] = {AudioBlock{outPtrs, 2, kFrames}};

    engine.render(inputs, outputs);

    CHECK(engine.scrubbedSamples() > 0);
    for (float sample : outLeft) CHECK(std::isfinite(sample));
    for (float sample : outRight) CHECK(std::isfinite(sample));
}

TEST_CASE("audio callbacks run with flush-to-zero and denormals-are-zero",
          "[sample][scrub][rt]") {
    // juce::ScopedNoDenormals sets both FTZ and DAZ for the duration of each
    // device callback, which is the render thread.
    const auto device = readSource("app/src/DeviceStream.cpp");
    const auto inputStart = device.find("void InputCapture::audioDeviceIOCallbackWithContext");
    const auto outputStart = device.find("void OutputPlayback::audioDeviceIOCallbackWithContext");
    REQUIRE(inputStart != std::string::npos);
    REQUIRE(outputStart != std::string::npos);
    INFO("input callback disables denormals");
    CHECK(device.find("juce::ScopedNoDenormals", inputStart) != std::string::npos);
    INFO("output callback disables denormals");
    CHECK(device.find("juce::ScopedNoDenormals", outputStart) != std::string::npos);
}
