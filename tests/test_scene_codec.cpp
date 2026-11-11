#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "pluginrack/SceneCodec.h"

TEST_CASE("scene survives a JSON round trip") {
    pluginrack::Scene original;
    original.schemaVersion = 5;
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
    ch1.colourHue = 190.0f;
    ch1.eqPhaseFlip = true;

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
    out2.colourHue = 270.0f;
    out2.eqPhaseFlip = true;

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
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(R"({"schemaVersion": 6})");
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
    expected.schemaVersion = 5;
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
    REQUIRE(channel.eqBands == pluginrack::defaultEqBands());
    REQUIRE(channel.eqDelayMs == 0.0f);
    REQUIRE(channel.trimDb == 0.0f);
    REQUIRE(loaded.scene->outputs.front().eqEnabled == false);
    REQUIRE(loaded.scene->outputs.front().eqBands == pluginrack::defaultEqBands());
    REQUIRE(loaded.scene->outputs.front().eqDelayMs == 0.0f);
    REQUIRE(loaded.scene->outputs.front().gainDb == 0.0f);
    REQUIRE(loaded.scene->outputs.front().muted == false);
}

TEST_CASE("slot plugin state is optional and survives a round trip") {
    pluginrack::Scene original;
    original.schemaVersion = 5;
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

TEST_CASE("deeply nested scene JSON is rejected before validation", "[scene][security]") {
    // PR-SEC-009: unbounded nesting exhausts the stack during parse or
    // teardown, before any validation runs. The SAX pre-pass rejects it.
    std::string deep(64, '[');
    deep += std::string(64, ']');
    const std::string json = "{\"schemaVersion\":5,\"channels\":" + deep + "}";
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(!loaded.scene.has_value());
    REQUIRE(!loaded.errors.empty());
}

TEST_CASE("scene JSON with unknown top-level keys is rejected", "[scene][security]") {
    const pluginrack::SceneLoadResult loaded =
        pluginrack::fromJson(R"({"schemaVersion":5,"evil":1})");
    REQUIRE(!loaded.scene.has_value());
    REQUIRE(!loaded.errors.empty());
}

TEST_CASE("oversized scene JSON is rejected before parsing", "[scene][security]") {
    const std::string big(pluginrack::kMaxSceneJsonBytes + 1, ' ');
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(big);
    REQUIRE(!loaded.scene.has_value());
    REQUIRE(!loaded.errors.empty());
}

TEST_CASE("slot plugin class id is optional and survives a round trip") {
    pluginrack::Scene original;
    original.schemaVersion = 5;
    original.name = "class-id";

    pluginrack::Channel channel;
    channel.id = 1;

    pluginrack::InsertSlot known;
    known.id = 4;
    known.pluginPath = "/plugins/drums.vst3";
    known.pluginName = "Kick";
    known.pluginUniqueId = 0x4B69636B;
    channel.inserts.push_back(known);

    pluginrack::InsertSlot unknown;
    unknown.id = 5;
    unknown.pluginPath = "/plugins/eq.dll";
    unknown.pluginName = "EQ";
    channel.inserts.push_back(unknown);

    original.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    output.name = "mains";
    original.outputs.push_back(output);

    const std::string json = pluginrack::toJson(original);
    REQUIRE(json.find("pluginUniqueId") != std::string::npos);

    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(*loaded.scene == original);
    REQUIRE(loaded.scene->channels.front().inserts[0].pluginUniqueId == 0x4B69636B);
    REQUIRE(loaded.scene->channels.front().inserts[1].pluginUniqueId == 0);
    // 0 is the "unknown" marker, so it is not written out.
    REQUIRE(pluginrack::toJson(*loaded.scene).find("\"pluginUniqueId\":0") == std::string::npos);
}

TEST_CASE("slots written before the class id existed load as unknown") {
    const std::string json = R"({
        "schemaVersion": 3,
        "name": "legacy-class",
        "sampleRate": 48000,
        "bufferFrames": 256,
        "channels": [{"id": 1, "inserts": [
            {"id": 9, "pluginPath": "/plugins/drums.vst3", "pluginName": "Kick"}
        ]}],
        "outputs": [{"id": 1, "name": "mains"}],
        "routes": []
    })";

    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->channels.front().inserts.front().pluginUniqueId == 0);
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
    // PR-SEC-009: unknown TOP-LEVEL keys are rejected (typo'd or foreign
    // files fail fast). Unknown keys nested inside known objects are still
    // ignored and dropped on save.
    const std::string json = R"({)"
                             R"("schemaVersion": 1,)"
                             R"("name": "future",)"
                             R"("sampleRate": 48000,)"
                             R"("bufferFrames": 256,)"
                             R"("channels": [],)"
                             R"("outputs": [],)"
                             R"("routes": [])"
                             R"(})";
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(pluginrack::toJson(*loaded.scene).find("futureThing") == std::string::npos);
}

TEST_CASE("unknown top-level keys are rejected", "[scene][security]") {
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
    REQUIRE(!loaded.scene.has_value());
    REQUIRE(!loaded.errors.empty());
}


TEST_CASE("inputChannels round-trips with the channel layout") {
    pluginrack::Scene original;
    original.schemaVersion = 5;
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
    // The factory EQ has seven points (high-pass through low-pass).
    REQUIRE(channel.eqBands.size() == 7);
    for (const pluginrack::EqBand& band : channel.eqBands) {
        REQUIRE(band.gainDb == 0.0f);
    }
    REQUIRE(channel.eqDelayMs == 0.0f);

    const pluginrack::Output& output = loaded.scene->outputs.front();
    REQUIRE(output.eqEnabled == false);
    REQUIRE(output.eqBands.size() == 7);
    for (const pluginrack::EqBand& band : output.eqBands) {
        REQUIRE(band.gainDb == 0.0f);
    }
    REQUIRE(output.eqDelayMs == 0.0f);

    // The loaded scene saves as v4, completing the auto-migration.
    REQUIRE(pluginrack::toJson(*loaded.scene).find("\"schemaVersion\":5") != std::string::npos);
}

TEST_CASE("scene v3 round-trip preserves eq bands") {
    pluginrack::Scene original;
    original.schemaVersion = 5;
    original.name = "eq";
    original.sampleRate = 48000;
    original.bufferFrames = 256;

    pluginrack::Channel channel;
    channel.id = 1;
    channel.name = "mic";
    channel.eqEnabled = true;
    channel.eqBands = pluginrack::defaultEqBands();
    channel.eqBands[0].enabled = false;
    channel.eqBands[3].freqHz = 1250.5f;
    channel.eqBands[3].gainDb = 6.0f;
    channel.eqBands[3].q = 1.2f;
    channel.eqBands[3].solo = true;
    channel.eqDelayMs = 12.5f;
    channel.eqRtaOffsetDb = -6.0f;
    channel.eqRtaWeighting = 2;
    original.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    output.name = "mains";
    output.eqEnabled = true;
    output.eqBands = pluginrack::defaultEqBands();
    output.eqBands[1].freqHz = 150.25f;
    output.eqBands[5].gainDb = -3.0f;
    output.eqDelayMs = 480.0f;
    output.eqRtaOffsetDb = 9.0f;
    output.eqRtaWeighting = 1;
    original.outputs.push_back(output);

    pluginrack::Route route;
    route.channelId = 1;
    route.outputId = 1;
    original.routes.push_back(route);

    const std::string json = pluginrack::toJson(original);
    REQUIRE(json.find("\"schemaVersion\":5") != std::string::npos);
    REQUIRE(json.find("eqBands") != std::string::npos);

    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(*loaded.scene == original);
    REQUIRE(loaded.scene->channels.front().eqBands[3].gainDb == 6.0f);
    REQUIRE(loaded.scene->channels.front().eqBands[3].solo == true);
    REQUIRE(loaded.scene->channels.front().eqBands[0].enabled == false);
    REQUIRE(loaded.scene->channels.front().eqDelayMs == 12.5f);
    REQUIRE(loaded.scene->channels.front().eqRtaOffsetDb == -6.0f);
    REQUIRE(loaded.scene->channels.front().eqRtaWeighting == 2);
    REQUIRE(loaded.scene->outputs.front().eqEnabled == true);
    REQUIRE(loaded.scene->outputs.front().eqBands[5].gainDb == -3.0f);
    REQUIRE(loaded.scene->outputs.front().eqDelayMs == 480.0f);
    REQUIRE(loaded.scene->outputs.front().eqRtaOffsetDb == 9.0f);
    REQUIRE(loaded.scene->outputs.front().eqRtaWeighting == 1);
}

TEST_CASE("scene audio settings round-trip") {
    pluginrack::Scene original;
    original.schemaVersion = 5;
    original.name = "tuned";

    pluginrack::AudioSettings audio;
    audio.inputBuffer = 128;
    audio.outputBuffer = 512;
    audio.latencyTuningOptIn = true;
    audio.lowLatencyInput = true;
    audio.minimalClockFifo = true;
    audio.autoReloadDevices = true;
    audio.rackThumbnailFps = 24;
    audio.rackPreviewMode = pluginrack::RackPreviewMode::Rotate;
    audio.watchedDevices.push_back(pluginrack::WatchedDevice{false, 2});
    audio.watchedDevices.push_back(pluginrack::WatchedDevice{true, 1});
    original.audio = audio;

    const std::string json = pluginrack::toJson(original);
    REQUIRE(json.find("\"audio\"") != std::string::npos);

    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->audio.has_value());
    REQUIRE(*loaded.scene->audio == audio);
    REQUIRE(*loaded.scene == original);
}

TEST_CASE("legacy audio tuning keys do not opt into beta latency modes") {
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(R"({
        "schemaVersion": 4,
        "audio": {
            "lowLatencyInput": true,
            "preferExclusive": true,
            "lowLatencyOutputs": true,
            "minimalClockFifo": true,
            "fullDuplexInput": true
        }
    })");
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->audio.has_value());
    const auto& audio = *loaded.scene->audio;
    CHECK_FALSE(audio.latencyTuningOptIn);
    CHECK_FALSE(audio.lowLatencyInput);
    CHECK_FALSE(audio.preferExclusive);
    CHECK_FALSE(audio.lowLatencyOutputs);
    CHECK_FALSE(audio.minimalClockFifo);
    CHECK_FALSE(audio.fullDuplexInput);
}

TEST_CASE("a scene without a preview mode keeps selected-only previews") {
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(R"({
        "schemaVersion": 5,
        "audio": {"rackThumbnailFps": 15}
    })");
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->audio.has_value());
    CHECK(loaded.scene->audio->rackPreviewMode == pluginrack::RackPreviewMode::Selected);
}

TEST_CASE("v5 scenes keep their tuning without the legacy opt-in gate") {
    pluginrack::Scene original;
    original.schemaVersion = 5;
    pluginrack::AudioSettings audio;
    audio.lowLatencyInput = true;
    audio.lowLatencyOutputs = true;
    original.audio = audio;

    const auto loaded = pluginrack::fromJson(pluginrack::toJson(original));
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->audio.has_value());
    CHECK(loaded.scene->audio->lowLatencyInput);
    CHECK(loaded.scene->audio->lowLatencyOutputs);
    // The two aggressive modes still need their own opt-in.
    CHECK_FALSE(loaded.scene->audio->minimalClockFifo);
    CHECK_FALSE(loaded.scene->audio->preferExclusive);
}

TEST_CASE("a scene written with the retired stream class still loads") {
    // The stream/monitor class is gone, but scenes saved with it must keep
    // loading: unknown keys are ignored, so the output comes back as a monitor.
    const auto loaded = pluginrack::fromJson(R"({
        "schemaVersion": 5,
        "outputs": [{"id": 1, "name": "mains", "streamOutput": true}]
    })");
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->outputs.size() == 1);
    CHECK(loaded.scene->outputs[0].name == "mains");

    pluginrack::Scene fresh;
    fresh.schemaVersion = 5;
    pluginrack::Output output;
    output.id = 1;
    output.name = "mains";
    fresh.outputs = {output};
    CHECK(pluginrack::toJson(fresh).find("streamOutput") == std::string::npos);
}

TEST_CASE("a scene without an audio block leaves the tuning unset") {    const pluginrack::SceneLoadResult loaded =
        pluginrack::fromJson(R"({"schemaVersion": 3})");
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(!loaded.scene->audio.has_value());
}

TEST_CASE("a split left and right send survives a round trip") {
    pluginrack::Scene original;
    original.schemaVersion = 5;
    original.name = "split-send";

    pluginrack::Channel channel;
    channel.id = 1;
    original.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    original.outputs.push_back(output);

    pluginrack::Route route;
    route.channelId = 1;
    route.outputId = 1;
    route.sendGainDb = -6.0f;
    route.sendGainDbR = -18.0f;
    original.routes.push_back(route);

    const std::string json = pluginrack::toJson(original);
    REQUIRE(json.find("\"sendGainDbR\":-18") != std::string::npos);

    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    REQUIRE(loaded.scene->routes.front().sendGainDb == -6.0f);
    REQUIRE(loaded.scene->routes.front().sendGainDbR.has_value());
    REQUIRE(*loaded.scene->routes.front().sendGainDbR == -18.0f);
    REQUIRE(*loaded.scene == original);
}

TEST_CASE("a matched send stores one level and stays compact") {
    pluginrack::Scene original;
    original.schemaVersion = 5;
    original.channels.push_back(pluginrack::Channel{});
    original.outputs.push_back(pluginrack::Output{});
    pluginrack::Route route;
    route.channelId = 0;
    route.outputId = 0;
    route.sendGainDb = -3.0f;
    original.routes.push_back(route);

    const std::string json = pluginrack::toJson(original);
    REQUIRE(json.find("sendGainDbR") == std::string::npos);

    // A scene written before the split restores a matched pair.
    const pluginrack::SceneLoadResult loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(!loaded.scene->routes.front().sendGainDbR.has_value());
}
