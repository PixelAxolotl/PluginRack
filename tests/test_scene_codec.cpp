#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "pluginrack/SceneCodec.h"

TEST_CASE("scene survives a JSON round trip") {
    pluginrack::Scene original;
    original.schemaVersion = 1;
    original.name = "roundtrip";
    original.sampleRate = 48000;
    original.bufferFrames = 256;

    pluginrack::Channel ch1;
    ch1.id = 1;
    ch1.name = "mic";
    ch1.sourceKind = pluginrack::SourceKind::Hardware;
    ch1.sourceDeviceId = "mic-1";
    ch1.layout = pluginrack::ChannelLayout::Mono;
    ch1.gainDb = 0.0f;
    ch1.pan = 0.0f;
    ch1.muted = false;
    ch1.soloed = false;

    pluginrack::Channel ch2;
    ch2.id = 2;
    ch2.name = "loopback";
    ch2.sourceKind = pluginrack::SourceKind::SystemLoopback;
    ch2.sourceDeviceId = "loop-1";
    ch2.layout = pluginrack::ChannelLayout::Stereo;
    ch2.gainDb = -6.0f;
    ch2.pan = 0.5f;
    ch2.muted = false;
    ch2.soloed = false;

    pluginrack::InsertSlot slot;
    slot.id = 7;
    slot.pluginPath = "/plugins/echo.dll";
    slot.pluginName = "Echo";
    slot.bypassed = true;
    ch2.inserts.push_back(slot);

    original.channels.push_back(ch1);
    original.channels.push_back(ch2);

    pluginrack::Output out1;
    out1.id = 1;
    out1.name = "mains";
    out1.kind = pluginrack::OutputKind::Physical;
    out1.deviceId = "out-1";

    pluginrack::Output out2;
    out2.id = 2;
    out2.name = "stream";
    out2.kind = pluginrack::OutputKind::VirtualOutput;
    out2.deviceId = "virt-1";

    original.outputs.push_back(out1);
    original.outputs.push_back(out2);

    pluginrack::Route r1;
    r1.channelId = 1;
    r1.outputId = 1;
    r1.sendGainDb = 0.0f;
    pluginrack::Route r2;
    r2.channelId = 1;
    r2.outputId = 2;
    r2.sendGainDb = -12.0f;
    pluginrack::Route r3;
    r3.channelId = 2;
    r3.outputId = 1;
    r3.sendGainDb = -6.0f;
    original.routes.push_back(r1);
    original.routes.push_back(r2);
    original.routes.push_back(r3);

    const std::string json = pluginrack::toJson(original);
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(*loaded.scene == original);
}

TEST_CASE("unsupported schema version is rejected") {
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(R"({"schemaVersion": 2})");
    REQUIRE(!loaded.scene.has_value());
    REQUIRE(loaded.errors.size() == 1);
    REQUIRE_THAT(loaded.errors[0], Catch::Matchers::ContainsSubstring("schemaVersion"));
}

TEST_CASE("unknown fields are ignored on load and dropped on save") {
    const std::string json = R"({)"
                             R"("schemaVersion": 1,)"
                             R"("name": "future",)"
                             R"("sampleRate": 48000,)"
                             R"("bufferFrames": 256,)"
                             R"("channels": [],)"
                             R"("outputs": [],)"
                             R"("routes": [],)"
                             R"("futureThing": {"a": 1}})";
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(pluginrack::toJson(*loaded.scene).find("futureThing") == std::string::npos);
}
