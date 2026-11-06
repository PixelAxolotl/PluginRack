#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pluginrack/Scene.h"
#include "pluginrack/SceneCodec.h"
#include "pluginrack/VirtualAudioProtocol.h"

TEST_CASE("virtual endpoint ids survive a scene round trip") {
    pluginrack::Scene scene;
    scene.name = "virtual";
    pluginrack::Channel channel;
    channel.id = 1;
    channel.name = "Game";
    channel.sourceKind = pluginrack::SourceKind::VirtualInput;
    channel.sourceDeviceId = pluginrack::virt::makeVirtualInputId(1);
    scene.channels.push_back(channel);
    pluginrack::Output output;
    output.id = 1;
    output.name = "Stream";
    output.kind = pluginrack::OutputKind::VirtualOutput;
    output.deviceId = pluginrack::virt::makeVirtualOutputId(1);
    scene.outputs.push_back(output);

    const std::string json = pluginrack::toJson(scene);
    const auto parsed = pluginrack::fromJson(json);
    REQUIRE(parsed.errors.empty());
    REQUIRE(parsed.scene.has_value());
    const auto& loaded = *parsed.scene;
    REQUIRE(loaded.channels.size() == 1);
    REQUIRE(loaded.outputs.size() == 1);
    CHECK(loaded.channels[0].sourceKind ==
          pluginrack::SourceKind::VirtualInput);
    CHECK(loaded.channels[0].sourceDeviceId ==
          "pluginrack://vaio/in/1");
    CHECK(loaded.outputs[0].kind == pluginrack::OutputKind::VirtualOutput);
    CHECK(loaded.outputs[0].deviceId == "pluginrack://vaio/out/1");
    // No schema bump: virtual kinds ride the existing fields.
    CHECK(loaded.schemaVersion == scene.schemaVersion);
}

TEST_CASE("virtual slot parsing rejects garbage without throwing") {
    CHECK(pluginrack::virt::parseVirtualSlot("") == -1);
    CHECK(pluginrack::virt::parseVirtualSlot("pluginrack://vaio/in/") == -1);
    CHECK(pluginrack::virt::parseVirtualSlot("pluginrack://vaio/in/x") == -1);
    CHECK(pluginrack::virt::parseVirtualSlot("ASIO::Realtek") == -1);
    CHECK(pluginrack::virt::isVirtualEndpointId(
              pluginrack::virt::makeVirtualInputId(2)));
    CHECK_FALSE(
        pluginrack::virt::isVirtualEndpointId("Windows Audio"));
}

TEST_CASE("virtual rate policy matches the drift corrector range") {
    // DriftResampler clamps at +-0.5%: the policy must agree.
    CHECK(pluginrack::virt::isRateCompatible(48000.0, 48000.0));
    CHECK(pluginrack::virt::isRateCompatible(48000.0, 48239.0));
    CHECK_FALSE(pluginrack::virt::isRateCompatible(48000.0, 48241.0));
    CHECK_FALSE(pluginrack::virt::isRateCompatible(48000.0, 44100.0));
}
