#include <catch2/catch_test_macros.hpp>

#include "pluginrack/Scene.h"
#include "pluginrack/SceneCodec.h"
#include "pluginrack/SceneValidator.h"

TEST_CASE("inputChannels mono mapping round-trips") {
    pluginrack::Scene original;
    original.schemaVersion = 2;
    original.name = "mono-map";
    original.sampleRate = 48000;
    original.bufferFrames = 256;
    pluginrack::Channel ch;
    ch.id = 1;
    ch.name = "mic";
    ch.layout = pluginrack::ChannelLayout::Mono;
    ch.inputChannels = {3};
    original.channels.push_back(ch);
    pluginrack::Output out; out.id = 1; out.name = "mains";
    original.outputs.push_back(out);
    const std::string json = pluginrack::toJson(original);
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels.front().inputChannels == std::vector<int>{3});
}

TEST_CASE("inputChannels stereo mapping round-trips") {
    pluginrack::Scene original;
    original.schemaVersion = 2;
    original.name = "stereo-map";
    original.sampleRate = 48000;
    original.bufferFrames = 256;
    pluginrack::Channel ch;
    ch.id = 1;
    ch.name = "mic";
    ch.layout = pluginrack::ChannelLayout::Stereo;
    ch.inputChannels = {2, 5};
    original.channels.push_back(ch);
    pluginrack::Output out; out.id = 1; out.name = "mains";
    original.outputs.push_back(out);
    const std::string json = pluginrack::toJson(original);
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels.front().inputChannels == std::vector<int>{2, 5});
}

TEST_CASE("old scene without inputChannels gets defaults") {
    const std::string json = R"({"schemaVersion":1,"name":"legacy","sampleRate":48000,"bufferFrames":256,"channels":[{"id":1,"name":"mic","layout":"Mono"},{"id":2,"name":"loop","layout":"Stereo"}],"outputs":[{"id":1,"name":"mains"}],"routes":[]})";
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels[0].inputChannels == std::vector<int>{0});
    REQUIRE(loaded.scene->channels[1].inputChannels == std::vector<int>{0, 1});
}

TEST_CASE("high physical input indexes round-trip and validate") {
    // Devices can expose more than the 16 inputs the old menus offered; the
    // mapping must keep the index the user picked instead of clamping it.
    pluginrack::Scene original;
    original.schemaVersion = 4;
    original.name = "high-inputs";
    original.sampleRate = 48000;
    original.bufferFrames = 256;
    pluginrack::Channel ch;
    ch.id = 1;
    ch.name = "adat";
    ch.layout = pluginrack::ChannelLayout::Stereo;
    ch.inputChannels = {20, 23};
    original.channels.push_back(ch);
    pluginrack::Output out;
    out.id = 1;
    out.name = "mains";
    original.outputs.push_back(out);

    REQUIRE(pluginrack::validate(original).empty());

    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(pluginrack::toJson(original));
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels.front().inputChannels == std::vector<int>{20, 23});
    REQUIRE(pluginrack::validate(*loaded.scene).empty());
}

TEST_CASE("invalid inputChannels mapping rejected") {
    pluginrack::Scene scene;
    scene.schemaVersion = 2;
    scene.name = "bad";
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;
    pluginrack::Channel ch;
    ch.id = 1;
    ch.name = "mic";
    ch.layout = pluginrack::ChannelLayout::Mono;
    ch.inputChannels = {0, 1};
    scene.channels.push_back(ch);
    pluginrack::Output out; out.id = 1; out.name = "mains";
    scene.outputs.push_back(out);
    const auto errors = pluginrack::validate(scene);
    REQUIRE(!errors.empty());
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
}