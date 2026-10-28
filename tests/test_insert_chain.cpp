#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pluginrack/InsertChain.h"
#include "pluginrack/MixerEngine.h"

#include "support/AllocCounter.h"
#include "support/TestPlugins.h"

namespace {

using pluginrack::AudioBlock;
using pluginrack::InsertSlot;

struct MonoBuffer {
    std::vector<float> samples;
    float* ptrs[1] = {nullptr};

    explicit MonoBuffer(int frames, float value = 0.0f)
        : samples(static_cast<std::size_t>(frames), value) {
        ptrs[0] = samples.data();
    }

    AudioBlock block() {
        return AudioBlock{ptrs, 1, static_cast<int>(samples.size())};
    }
};

InsertSlot makeSlot(std::uint32_t id, const std::string& path, bool bypassed = false) {
    InsertSlot slot;
    slot.id = id;
    slot.pluginPath = path;
    slot.pluginName = "test-plugin-" + std::to_string(id);
    slot.bypassed = bypassed;
    return slot;
}

class AddThenHalveFactory : public pluginrack::IPluginFactory {
public:
    std::unique_ptr<pluginrack::IPluginInstance> create(
        const std::string& pluginPath, std::string* error) override {
        if (error != nullptr) {
            error->clear();
        }
        if (pluginPath == "/add") {
            return std::make_unique<pluginrack::test::AddOnePlugin>();
        }
        return std::make_unique<pluginrack::test::HalvePlugin>();
    }
};

class FailPathFactory : public pluginrack::IPluginFactory {
public:
    std::unique_ptr<pluginrack::IPluginInstance> create(
        const std::string& pluginPath, std::string* error) override {
        if (pluginPath == "/bad") {
            if (error != nullptr) {
                *error = "boom";
            }
            return nullptr;
        }
        if (error != nullptr) {
            error->clear();
        }
        return std::make_unique<pluginrack::test::HalvePlugin>();
    }
};

class FailProcessPlugin : public pluginrack::IPluginInstance {
public:
    explicit FailProcessPlugin(int* calls) : calls_(calls) {}

    void prepare(double /*sampleRate*/, int /*maxBlockFrames*/) override {}
    pluginrack::ProcessResult process(pluginrack::AudioBlock& /*block*/) noexcept override {
        ++*calls_;
        return pluginrack::ProcessResult::Failed;
    }
    void reset() noexcept override {}

private:
    int* calls_;
};

class FailProcessFactory : public pluginrack::IPluginFactory {
public:
    explicit FailProcessFactory(int* calls) : calls_(calls) {}

    std::unique_ptr<pluginrack::IPluginInstance> create(
        const std::string& /*pluginPath*/, std::string* error) override {
        if (error != nullptr) {
            error->clear();
        }
        return std::make_unique<FailProcessPlugin>(calls_);
    }

private:
    int* calls_;
};

} // namespace

TEST_CASE("inserts run in order") {
    AddThenHalveFactory factory;
    const std::vector<InsertSlot> slots = {makeSlot(1, "/add"), makeSlot(2, "/halve")};
    pluginrack::InsertChain chain(slots, factory, 48000.0, 256);

    MonoBuffer buffer(8, 0.0f);
    AudioBlock block = buffer.block();
    chain.process(block);

    // (0.0 + 0.25) * 0.5 = 0.125
    for (float sample : buffer.samples) {
        REQUIRE(sample == Catch::Approx(0.125f));
    }
    REQUIRE(chain.failedSlots().empty());
}

TEST_CASE("bypassed insert is skipped") {
    pluginrack::test::HalfFactory factory;
    const std::vector<InsertSlot> slots = {makeSlot(1, "/halve", true)};
    pluginrack::InsertChain chain(slots, factory, 48000.0, 256);

    MonoBuffer buffer(8, 1.0f);
    AudioBlock block = buffer.block();
    chain.process(block);

    for (float sample : buffer.samples) {
        REQUIRE(sample == Catch::Approx(1.0f));
    }
    REQUIRE(chain.failedSlots().empty());
}

TEST_CASE("failed plugin load is reported and skipped") {
    FailPathFactory factory;
    const std::vector<InsertSlot> slots = {
        makeSlot(1, "/good1"), makeSlot(2, "/bad"), makeSlot(3, "/good2")};
    pluginrack::InsertChain chain(slots, factory, 48000.0, 256);

    REQUIRE(chain.failedSlots() == std::vector<std::uint32_t>{2});

    MonoBuffer buffer(8, 1.0f);
    AudioBlock block = buffer.block();
    chain.process(block);

    // The two surviving Halve inserts run in order: 1.0 * 0.5 * 0.5 = 0.25.
    for (float sample : buffer.samples) {
        REQUIRE(sample == Catch::Approx(0.25f));
    }
}

TEST_CASE("processing failure permanently bypasses the slot") {
    int calls = 0;
    FailProcessFactory factory(&calls);
    const std::vector<InsertSlot> slots = {makeSlot(9, "/fails-during-processing")};
    pluginrack::InsertChain chain(slots, factory, 48000.0, 256);

    MonoBuffer buffer(8, 1.0f);
    AudioBlock block = buffer.block();
    AllocCounter::reset();
    chain.process(block);
    chain.process(block);

    REQUIRE(calls == 1);
    REQUIRE(AllocCounter::allocations() == 0);
    REQUIRE(chain.failedSlots() == std::vector<std::uint32_t>{9});
}

TEST_CASE("chain processing allocates nothing") {
    pluginrack::test::HalfFactory factory;
    const std::vector<InsertSlot> slots = {makeSlot(1, "/halve"), makeSlot(2, "/halve")};
    pluginrack::InsertChain chain(slots, factory, 48000.0, 256);

    MonoBuffer buffer(8, 1.0f);
    AudioBlock block = buffer.block();
    AllocCounter::reset();
    chain.process(block);

    REQUIRE(AllocCounter::allocations() == 0);
}

TEST_CASE("engine applies the insert chain before the fader") {
    pluginrack::Scene scene;
    scene.schemaVersion = 1;
    scene.name = "inserts";
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;

    pluginrack::Channel channel;
    channel.id = 1;
    channel.name = "ch1";
    channel.layout = pluginrack::ChannelLayout::Mono;
    channel.gainDb = 0.0f;
    channel.pan = 0.0f;
    channel.inserts.push_back(makeSlot(1, "/halve"));
    scene.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    output.name = "out1";
    scene.outputs.push_back(output);

    pluginrack::Route route;
    route.channelId = 1;
    route.outputId = 1;
    route.sendGainDb = 0.0f;
    scene.routes.push_back(route);

    pluginrack::test::HalfFactory factory;
    pluginrack::MixerEngine engine;
    engine.setPluginFactory(&factory);
    std::string error;
    REQUIRE(engine.prepare(scene, 256, &error));

    constexpr int kFrames = 128;
    std::vector<float> mono(static_cast<std::size_t>(kFrames), 1.0f);
    float* inPtrs[1] = {mono.data()};
    const AudioBlock inputs[] = {AudioBlock{inPtrs, 1, kFrames}};

    std::vector<float> outLeft(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> outRight(static_cast<std::size_t>(kFrames), 0.0f);
    float* outPtrs[2] = {outLeft.data(), outRight.data()};
    AudioBlock outputs[] = {AudioBlock{outPtrs, 2, kFrames}};

    engine.render(inputs, outputs);

    // Halve (x0.5) runs before the center-pan fader (x0.70710678).
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(outLeft[static_cast<std::size_t>(i)] == Catch::Approx(0.5f * 0.70710678f));
        REQUIRE(outRight[static_cast<std::size_t>(i)] == Catch::Approx(0.5f * 0.70710678f));
    }
}

TEST_CASE("engine reports plugin slots that failed to load") {
    pluginrack::Scene scene;
    scene.schemaVersion = 1;
    scene.name = "failed-insert";
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;

    pluginrack::Channel channel;
    channel.id = 1;
    channel.name = "ch1";
    channel.inserts.push_back(makeSlot(7, "/bad"));
    scene.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    output.name = "out1";
    scene.outputs.push_back(output);

    pluginrack::Route route;
    route.channelId = 1;
    route.outputId = 1;
    scene.routes.push_back(route);

    FailPathFactory factory;
    pluginrack::MixerEngine engine;
    engine.setPluginFactory(&factory);
    std::string error;
    REQUIRE(engine.prepare(scene, 256, &error));
    REQUIRE(engine.failedPluginSlots() == std::vector<std::uint32_t>{7});
}
