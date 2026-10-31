#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "pluginrack/SceneCodec.h"

TEST_CASE("scene survives a JSON round trip") {
    pluginrack::Scene original;
    original.schemaVersion = 3;
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
    ch1.trimDb = -3.0f;
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
    slot.pluginState = "AAECAwQ="; // base64 of 00 01 02 03 04
    ch2.inserts.push_back(slot);

    original.channels.push_back(ch1);
    original.channels.push_back(ch2);

    pluginrack::Output out1;
    out1.id = 1;
    out1.name = "mains";
    out1.kind = pluginrack::OutputKind::Physical;
    out1.deviceId = "out-1";
    out1.gainDb = -6.0f;
    out1.muted = true;

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
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(R"({"schemaVersion": 4})");
    REQUIRE(!loaded.scene.has_value());
    REQUIRE(loaded.errors.size() == 1);
    REQUIRE_THAT(loaded.errors[0], Catch::Matchers::ContainsSubstring("schemaVersion"));
}

TEST_CASE("v2 scene with output inserts round-trips") {
    pluginrack::Scene original;
    original.schemaVersion = 2;
    original.name = "outfx";
    original.sampleRate = 48000;
    original.bufferFrames = 256;

    pluginrack::Channel channel;
    channel.id = 1;
    channel.name = "mic";
    original.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    output.name = "mains";
    output.kind = pluginrack::OutputKind::Physical;
    output.deviceId = "out-1";

    pluginrack::InsertSlot slot1;
    slot1.id = 7;
    slot1.pluginPath = "/plugins/echo.dll";
    slot1.pluginName = "Echo";
    slot1.bypassed = true;
    output.inserts.push_back(slot1);

    pluginrack::InsertSlot slot2;
    slot2.id = 8;
    slot2.pluginPath = "/plugins/gain.dll";
    slot2.pluginName = "Gain";
    slot2.bypassed = false;
    output.inserts.push_back(slot2);

    original.outputs.push_back(output);

    pluginrack::Route route;
    route.channelId = 1;
    route.outputId = 1;
    route.sendGainDb = 0.0f;
    original.routes.push_back(route);

    const std::string json = pluginrack::toJson(original);
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    // v2 auto-migrates: saving a v2 scene always writes a v3 file.
    pluginrack::Scene expected = original;
    expected.schemaVersion = 3;
    REQUIRE(*loaded.scene == expected);
    REQUIRE(loaded.scene->outputs.front().inserts.size() == 2);
    REQUIRE(loaded.scene->outputs.front().inserts[0].pluginPath == "/plugins/echo.dll");
    REQUIRE(loaded.scene->outputs.front().inserts[1].bypassed == false);
}

TEST_CASE("v1 scene loads with empty output racks and no errors") {
    const std::string json = R"({)"
                             R"("schemaVersion": 1,)"
                             R"("name": "legacy",)"
                             R"("sampleRate": 48000,)"
                             R"("bufferFrames": 256,)"
                             R"("channels": [{"id": 1}],)"
                             R"("outputs": [{"id": 1, "name": "mains"}],)"
                             R"("routes": [])"
                             R"(})";
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->outputs.size() == 1);
    REQUIRE(loaded.scene->outputs.front().inserts.empty());

    // Task 6 Step 2 gap fill: v1 predates the EQ, so loading it must yield
    // the same flat default bands as v2 (mirrors "v2 scene loads flat EQ").
    const pluginrack::Channel& channel = loaded.scene->channels.front();
    REQUIRE(channel.eqEnabled == false);
    REQUIRE(channel.eqBands == pluginrack::defaultSixBands());
    REQUIRE(channel.eqDelayMs == 0.0f);
    REQUIRE(channel.trimDb == 0.0f);
    REQUIRE(loaded.scene->outputs.front().eqEnabled == false);
    REQUIRE(loaded.scene->outputs.front().eqBands == pluginrack::defaultSixBands());
    REQUIRE(loaded.scene->outputs.front().eqDelayMs == 0.0f);
    REQUIRE(loaded.scene->outputs.front().gainDb == 0.0f);
    REQUIRE(loaded.scene->outputs.front().muted == false);
}

TEST_CASE("slot plugin state is optional and survives a round trip") {
    pluginrack::Scene original;
    original.schemaVersion = 3;
    original.name = "state";

    pluginrack::Channel channel;
    channel.id = 1;

    pluginrack::InsertSlot withState;
    withState.id = 4;
    withState.pluginPath = "/plugins/eq.dll";
    withState.pluginName = "EQ";
    withState.pluginState = "AAECAwQFBgc=";
    channel.inserts.push_back(withState);

    pluginrack::InsertSlot withoutState;
    withoutState.id = 5;
    withoutState.pluginPath = "/plugins/gain.dll";
    withoutState.pluginName = "Gain";
    channel.inserts.push_back(withoutState);

    original.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    output.name = "mains";
    pluginrack::InsertSlot outputSlot;
    outputSlot.id = 6;
    outputSlot.pluginPath = "/plugins/limiter.dll";
    outputSlot.pluginName = "Limiter";
    outputSlot.pluginState = "bnVsbA==";
    output.inserts.push_back(outputSlot);
    original.outputs.push_back(output);

    const std::string json = pluginrack::toJson(original);
    REQUIRE(json.find("pluginState") != std::string::npos);

    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(*loaded.scene == original);
    REQUIRE(loaded.scene->channels.front().inserts[0].pluginState == "AAECAwQFBgc=");
    REQUIRE(loaded.scene->channels.front().inserts[1].pluginState.empty());
    REQUIRE(loaded.scene->outputs.front().inserts[0].pluginState == "bnVsbA==");
}

TEST_CASE("v3 slots without plugin state load with empty state") {
    const std::string json = R"({
        "schemaVersion": 3,
        "name": "legacy-state",
        "sampleRate": 48000,
        "bufferFrames": 256,
        "channels": [{"id": 1, "inserts": [
            {"id": 9, "pluginPath": "/plugins/eq.dll", "pluginName": "EQ", "bypassed": false}
        ]}],
        "outputs": [{"id": 1, "name": "mains"}],
        "routes": []
    })";

    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels.front().inserts.size() == 1);
    REQUIRE(loaded.scene->channels.front().inserts.front().pluginState.empty());
    // No empty key is emitted for slots that never had state.
    REQUIRE(pluginrack::toJson(*loaded.scene).find("pluginState") == std::string::npos);
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


TEST_CASE("inputChannels round-trips with the channel layout") {
    pluginrack::Scene original;
    original.schemaVersion = 3;
    original.name = "mapping";

    pluginrack::Channel mono;
    mono.id = 1;
    mono.name = "kick";
    mono.layout = pluginrack::ChannelLayout::Mono;
    mono.inputChannels = {2};

    pluginrack::Channel stereo;
    stereo.id = 2;
    stereo.name = "keys";
    stereo.layout = pluginrack::ChannelLayout::Stereo;
    stereo.inputChannels = {4, 3};

    original.channels.push_back(mono);
    original.channels.push_back(stereo);

    const std::string json = pluginrack::toJson(original);
    REQUIRE(json.find("inputChannels") != std::string::npos);

    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(*loaded.scene == original);
    REQUIRE(loaded.scene->channels[0].inputChannels == std::vector<int>{2});
    REQUIRE(loaded.scene->channels[1].inputChannels == std::vector<int>{4, 3});
}

TEST_CASE("scenes without inputChannels load with layout defaults") {
    const std::string json = R"({)"
                             R"("schemaVersion": 2,)"
                             R"("name": "legacy",)"
                             R"("sampleRate": 48000,)"
                             R"("bufferFrames": 256,)"
                             R"("channels": [)"
                             R"({"id": 1, "name": "mic", "layout": "Mono"},)"
                             R"({"id": 2, "name": "synth", "layout": "Stereo"},)"
                             R"({"id": 3, "name": "default"})"
                             R"(],)"
                             R"("outputs": [{"id": 1, "name": "mains"}],)"
                             R"("routes": []})";
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels.size() == 3);
    REQUIRE(loaded.scene->channels[0].inputChannels == std::vector<int>{0});
    REQUIRE(loaded.scene->channels[1].inputChannels == std::vector<int>{0, 1});
    REQUIRE(loaded.scene->channels[2].inputChannels == std::vector<int>{0});
}

TEST_CASE("v2 scene loads flat EQ and saves as v3") {
    const std::string json = R"({)"
                             R"("schemaVersion": 2,)"
                             R"("name": "legacy-eq",)"
                             R"("sampleRate": 48000,)"
                             R"("bufferFrames": 256,)"
                             R"("channels": [{"id": 1, "name": "mic"}],)"
                             R"("outputs": [{"id": 1, "name": "mains"}],)"
                             R"("routes": [])"
                             R"(})";
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->schemaVersion == 2);

    const pluginrack::Channel& channel = loaded.scene->channels.front();
    REQUIRE(channel.eqEnabled == false);
    REQUIRE(channel.eqBands.size() == 6);
    for (const pluginrack::EqBand& band : channel.eqBands) {
        REQUIRE(band.gainDb == 0.0f);
    }
    REQUIRE(channel.eqDelayMs == 0.0f);

    const pluginrack::Output& output = loaded.scene->outputs.front();
    REQUIRE(output.eqEnabled == false);
    REQUIRE(output.eqBands.size() == 6);
    for (const pluginrack::EqBand& band : output.eqBands) {
        REQUIRE(band.gainDb == 0.0f);
    }
    REQUIRE(output.eqDelayMs == 0.0f);

    // The loaded scene saves as v3, completing the auto-migration.
    REQUIRE(pluginrack::toJson(*loaded.scene).find("\"schemaVersion\":3") != std::string::npos);
}

TEST_CASE("scene v3 round-trip preserves eq bands") {
    pluginrack::Scene original;
    original.schemaVersion = 3;
    original.name = "eq";
    original.sampleRate = 48000;
    original.bufferFrames = 256;

    pluginrack::Channel channel;
    channel.id = 1;
    channel.name = "mic";
    channel.eqEnabled = true;
    channel.eqBands = pluginrack::defaultSixBands();
    channel.eqBands[0].enabled = false;
    channel.eqBands[3].freqHz = 1250.5f;
    channel.eqBands[3].gainDb = 6.0f;
    channel.eqBands[3].q = 1.2f;
    channel.eqBands[3].solo = true;
    channel.eqDelayMs = 12.5f;
    original.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    output.name = "mains";
    output.eqEnabled = true;
    output.eqBands = pluginrack::defaultSixBands();
    output.eqBands[1].freqHz = 150.25f;
    output.eqBands[5].gainDb = -3.0f;
    output.eqDelayMs = 480.0f;
    original.outputs.push_back(output);

    pluginrack::Route route;
    route.channelId = 1;
    route.outputId = 1;
    original.routes.push_back(route);

    const std::string json = pluginrack::toJson(original);
    REQUIRE(json.find("\"schemaVersion\":3") != std::string::npos);
    REQUIRE(json.find("eqBands") != std::string::npos);

    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(*loaded.scene == original);
    REQUIRE(loaded.scene->channels.front().eqBands[3].gainDb == 6.0f);
    REQUIRE(loaded.scene->channels.front().eqBands[3].solo == true);
    REQUIRE(loaded.scene->channels.front().eqBands[0].enabled == false);
    REQUIRE(loaded.scene->channels.front().eqDelayMs == 12.5f);
    REQUIRE(loaded.scene->outputs.front().eqEnabled == true);
    REQUIRE(loaded.scene->outputs.front().eqBands[5].gainDb == -3.0f);
    REQUIRE(loaded.scene->outputs.front().eqDelayMs == 480.0f);
}
