#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "pluginrack/SceneLimits.h"
#include "pluginrack/SceneValidator.h"

namespace {

pluginrack::Scene makeValidScene() {
    pluginrack::Scene scene;
    scene.schemaVersion = 1;
    scene.name = "valid";
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;

    pluginrack::Channel channel;
    channel.id = 1;
    channel.name = "mic";
    scene.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    output.name = "mains";
    scene.outputs.push_back(output);

    pluginrack::Route route;
    route.channelId = 1;
    route.outputId = 1;
    scene.routes.push_back(route);

    return scene;
}

} // namespace

TEST_CASE("valid scene produces no errors") {
    const pluginrack::Scene scene = makeValidScene();
    REQUIRE(pluginrack::validate(scene).empty());
}

TEST_CASE("duplicate route pair is rejected") {
    pluginrack::Scene scene = makeValidScene();
    scene.routes.push_back(scene.routes.front());
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::DuplicateRoute);
}

TEST_CASE("route to missing output is rejected") {
    pluginrack::Scene scene = makeValidScene();
    scene.routes.front().outputId = 99;
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::UnknownOutputInRoute);
    REQUIRE(errors[0].entityId == 99);
}

TEST_CASE("out of range values are reported") {
    pluginrack::Scene scene = makeValidScene();
    scene.channels.front().gainDb = 20.0f;
    scene.channels.front().pan = -2.0f;
    scene.routes.front().sendGainDb = -100.0f;
    scene.sampleRate = 44101;
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 4);
    for (const pluginrack::SceneError& error : errors) {
        REQUIRE(error.code == pluginrack::SceneErrorCode::ValueOutOfRange);
    }
}

TEST_CASE("out of range trim and output gain are reported") {
    pluginrack::Scene scene = makeValidScene();
    scene.channels.front().trimDb = 13.0f;
    scene.outputs.front().gainDb = -61.0f;
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 2);
    for (const pluginrack::SceneError& error : errors) {
        REQUIRE(error.code == pluginrack::SceneErrorCode::ValueOutOfRange);
    }
    REQUIRE(errors[0].message.find("trimDb") != std::string::npos);
    REQUIRE(errors[1].message.find("gainDb") != std::string::npos);
}

TEST_CASE("scene with no outputs is rejected") {
    pluginrack::Scene scene = makeValidScene();
    scene.outputs.clear();
    scene.routes.clear();
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::NoOutputs);
}

TEST_CASE("unsupported schema version is rejected by validation") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = pluginrack::kSchemaVersion + 1;
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::UnsupportedSchemaVersion);
    // The message must name the range the validator actually accepts.
    REQUIRE(errors[0].message.find("expected 1 to " + std::to_string(pluginrack::kSchemaVersion)) !=
            std::string::npos);
}

TEST_CASE("scene rejects an out-of-range right send level") {
    pluginrack::Scene scene = makeValidScene();
    scene.routes.front().sendGainDbR = -100.0f;
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].message.find("sendGainDbR") != std::string::npos);
}

TEST_CASE("v3 scene with default flat EQ is valid") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = 3;
    scene.channels.front().eqEnabled = true;
    scene.outputs.front().eqEnabled = true;
    REQUIRE(pluginrack::validate(scene).empty());
}

TEST_CASE("scene rejects out-of-range eq band parameters") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = 3;
    scene.channels.front().eqEnabled = true;

    SECTION("freq below floor") {
        scene.channels.front().eqBands[2].freqHz = 5.0f;
    }
    SECTION("freq above ceiling") {
        scene.channels.front().eqBands[2].freqHz = 25000.0f;
    }
    SECTION("non-finite freq") {
        scene.channels.front().eqBands[2].freqHz = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("gain above ceiling") {
        scene.channels.front().eqBands[0].gainDb = 16.0f;
    }
    SECTION("gain below floor") {
        scene.channels.front().eqBands[0].gainDb = -16.0f;
    }
    SECTION("Q below floor") {
        scene.channels.front().eqBands[4].q = 0.05f;
    }
    SECTION("Q above ceiling") {
        scene.channels.front().eqBands[4].q = 21.0f;
    }

    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].entityId == 1);
    REQUIRE(errors[0].message.find("eqBands") != std::string::npos);
}

TEST_CASE("scene rejects out-of-range output eq bands") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = 3;
    scene.outputs.front().eqEnabled = true;
    scene.outputs.front().eqBands[1].gainDb = 20.0f;
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].entityId == 1);
    REQUIRE(errors[0].message.find("eqBands") != std::string::npos);
}

TEST_CASE("scene rejects an eq slope that is not 12, 24 or 48") {
    pluginrack::Scene scene = makeValidScene();
    scene.channels.front().eqEnabled = true;

    SECTION("channel 13 dB/oct") {
        scene.channels.front().eqBands[0].slopeDbPerOct = 13;
    }
    SECTION("channel zero") {
        scene.channels.front().eqBands[0].slopeDbPerOct = 0;
    }
    SECTION("output 36 dB/oct") {
        scene.outputs.front().eqEnabled = true;
        scene.outputs.front().eqBands[6].slopeDbPerOct = 36;
    }

    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].message.find("slopeDbPerOct") != std::string::npos);
}

TEST_CASE("scene accepts every eq slope") {
    pluginrack::Scene scene = makeValidScene();
    scene.channels.front().eqEnabled = true;
    for (const int slope : { 12, 24, 48 }) {
        scene.channels.front().eqBands[0].slopeDbPerOct = slope;
        REQUIRE(pluginrack::validate(scene).empty());
    }
}

TEST_CASE("scene accepts eq delay at both ends of the range") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = 3;
    scene.channels.front().eqEnabled = true;
    scene.channels.front().eqDelayMs = 1000.0f;
    scene.outputs.front().eqEnabled = true;
    scene.outputs.front().eqDelayMs = 0.0f;
    REQUIRE(pluginrack::validate(scene).empty());
}

TEST_CASE("scene rejects out-of-range eq delay") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = 3;
    scene.channels.front().eqEnabled = true;
    scene.outputs.front().eqEnabled = true;

    SECTION("channel above ceiling") {
        scene.channels.front().eqDelayMs = 1000.5f;
    }
    SECTION("channel below floor") {
        scene.channels.front().eqDelayMs = -0.5f;
    }
    SECTION("channel non-finite") {
        scene.channels.front().eqDelayMs = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("output above ceiling") {
        scene.outputs.front().eqDelayMs = 2000.0f;
    }

    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].entityId == 1);
    REQUIRE(errors[0].message.find("eqDelayMs") != std::string::npos);
}

TEST_CASE("zero entity ids are rejected") {
    pluginrack::Scene scene = makeValidScene();

    SECTION("channel") {
        scene.channels.front().id = 0;
        scene.routes.clear();
    }
    SECTION("output") {
        scene.outputs.front().id = 0;
        scene.routes.clear();
    }
    SECTION("insert slot") {
        scene.channels.front().inserts.push_back(pluginrack::InsertSlot{});
    }

    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
}

TEST_CASE("zero route endpoints are rejected in endpoint order") {
    pluginrack::Scene scene = makeValidScene();
    scene.routes.front().channelId = 0;
    scene.routes.front().outputId = 0;
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 2);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[1].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].message.find("channelId") != std::string::npos);
    REQUIRE(errors[1].message.find("outputId") != std::string::npos);
}

TEST_CASE("non-finite mixer parameters are rejected") {
    pluginrack::Scene scene = makeValidScene();

    SECTION("NaN gain") {
        scene.channels.front().gainDb = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("infinite pan") {
        scene.channels.front().pan = std::numeric_limits<float>::infinity();
    }
    SECTION("infinite send gain") {
        scene.routes.front().sendGainDb = -std::numeric_limits<float>::infinity();
    }

    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
}

TEST_CASE("dynamics parameters are finite and within the UI ranges") {
    pluginrack::Scene scene = makeValidScene();

    SECTION("makeup above ceiling") {
        scene.channels.front().effects.front().makeupDb = 25.0f;
    }
    SECTION("input gain below floor") {
        scene.channels.front().effects.front().inputGainDb = -25.0f;
    }
    SECTION("non-finite knee") {
        scene.channels.front().effects.front().kneeDb =
            std::numeric_limits<float>::quiet_NaN();
    }

    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].message.find("dynamics") != std::string::npos);
}


TEST_CASE("valid stereo input mapping produces no errors") {
    pluginrack::Scene scene = makeValidScene();
    scene.channels.front().layout = pluginrack::ChannelLayout::Stereo;
    scene.channels.front().inputChannels = {1, 0};
    REQUIRE(pluginrack::validate(scene).empty());
}

TEST_CASE("invalid input mappings are rejected") {
    pluginrack::Scene scene = makeValidScene();
    pluginrack::Channel& channel = scene.channels.front();

    SECTION("mono without indexes") {
        channel.inputChannels.clear();
    }
    SECTION("mono with two indexes") {
        channel.inputChannels = {0, 1};
    }
    SECTION("mono with negative index") {
        channel.inputChannels = {-1};
    }
    SECTION("stereo with one index") {
        channel.layout = pluginrack::ChannelLayout::Stereo;
        channel.inputChannels = {3};
    }
    SECTION("stereo with three indexes") {
        channel.layout = pluginrack::ChannelLayout::Stereo;
        channel.inputChannels = {0, 1, 2};
    }
    SECTION("stereo with negative index") {
        channel.layout = pluginrack::ChannelLayout::Stereo;
        channel.inputChannels = {0, -2};
    }
    SECTION("stereo with duplicate indexes") {
        channel.layout = pluginrack::ChannelLayout::Stereo;
        channel.inputChannels = {1, 1};
    }

    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].entityId == 1);
    REQUIRE(errors[0].message.find("inputChannels") != std::string::npos);
}

TEST_CASE("input mapping error names the channel and mapping") {
    pluginrack::Scene scene = makeValidScene();
    scene.channels.front().layout = pluginrack::ChannelLayout::Stereo;
    scene.channels.front().inputChannels = {2, 2};
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].message.find("channel 1") != std::string::npos);
    REQUIRE(errors[0].message.find("[2, 2]") != std::string::npos);
}

TEST_CASE("scene with no channels is rejected") {
    pluginrack::Scene scene = makeValidScene();
    scene.channels.clear();
    scene.routes.clear();
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::NoChannels);
}

TEST_CASE("buffer size is bounded") {
    pluginrack::Scene scene = makeValidScene();

    SECTION("below floor") {
        scene.bufferFrames = 16;
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
        REQUIRE(errors[0].message.find("bufferFrames") != std::string::npos);
    }
    SECTION("above ceiling") {
        scene.bufferFrames = 16384;
        REQUIRE(pluginrack::validate(scene).size() == 1);
    }
    SECTION("both ends of the range are accepted") {
        // 32 is a real ASIO block size (M-Audio M-Track), so a scene saved at
        // 32 samples must validate.
        scene.bufferFrames = 32;
        REQUIRE(pluginrack::validate(scene).empty());
        scene.bufferFrames = 8192;
        REQUIRE(pluginrack::validate(scene).empty());
    }
}

TEST_CASE("analyzer settings are bounded") {
    pluginrack::Scene scene = makeValidScene();

    SECTION("weighting above the known curves") {
        scene.channels.front().eqRtaWeighting = 3;
    }
    SECTION("negative weighting") {
        scene.outputs.front().eqRtaWeighting = -1;
    }
    SECTION("offset above ceiling") {
        scene.channels.front().eqRtaOffsetDb = 61.0f;
    }
    SECTION("offset below floor") {
        scene.outputs.front().eqRtaOffsetDb = -61.0f;
    }
    SECTION("non-finite offset") {
        scene.channels.front().eqRtaOffsetDb = std::numeric_limits<float>::quiet_NaN();
    }

    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
}

TEST_CASE("dynamics parameters are bounded at the top of the UI ranges") {
    pluginrack::Scene scene = makeValidScene();
    pluginrack::DynamicsEffect& effect = scene.channels.front().effects.front();

    SECTION("threshold above 0 dB") {
        effect.thresholdDb = 1.0f;
    }
    SECTION("ratio above the gate range") {
        effect.ratio = 81.0f;
    }
    SECTION("attack above the knob range") {
        effect.attackMs = 201.0f;
    }
    SECTION("release above the knob range") {
        effect.releaseMs = 1001.0f;
    }

    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].message.find("dynamics") != std::string::npos);
}

TEST_CASE("gate and compressor depth fields are bounded") {
    pluginrack::Scene scene = makeValidScene();
    pluginrack::DynamicsEffect& effect = scene.channels.front().effects.front();

    SECTION("hold above 2000 ms") {
        effect.holdMs = 2001.0f;
    }
    SECTION("negative hysteresis") {
        effect.hysteresisDb = -1.0f;
    }
    SECTION("side-chain above 1000 Hz") {
        effect.sidechainHz = 1001.0f;
    }
    SECTION("mix above 100 percent") {
        effect.mixPercent = 101.0f;
    }
    SECTION("non-finite hold") {
        effect.holdMs = std::numeric_limits<float>::quiet_NaN();
    }

    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].message.find("dynamics") != std::string::npos);
}

TEST_CASE("gate and compressor depth errors name the offending field") {
    pluginrack::Scene scene = makeValidScene();
    pluginrack::DynamicsEffect& effect = scene.channels.front().effects.front();

    effect.holdMs = 2001.0f;
    const std::vector<pluginrack::SceneError> holdErrors = pluginrack::validate(scene);
    REQUIRE(holdErrors.size() == 1);
    REQUIRE(holdErrors[0].message.find("holdMs") != std::string::npos);

    effect.holdMs = 0.0f;
    effect.hysteresisDb = 25.0f;
    const std::vector<pluginrack::SceneError> hysteresisErrors = pluginrack::validate(scene);
    REQUIRE(hysteresisErrors.size() == 1);
    REQUIRE(hysteresisErrors[0].message.find("hysteresisDb") != std::string::npos);

    effect.hysteresisDb = 0.0f;
    effect.sidechainHz = 1001.0f;
    const std::vector<pluginrack::SceneError> sidechainErrors = pluginrack::validate(scene);
    REQUIRE(sidechainErrors.size() == 1);
    REQUIRE(sidechainErrors[0].message.find("sidechainHz") != std::string::npos);

    effect.sidechainHz = 0.0f;
    effect.mixPercent = 101.0f;
    const std::vector<pluginrack::SceneError> mixErrors = pluginrack::validate(scene);
    REQUIRE(mixErrors.size() == 1);
    REQUIRE(mixErrors[0].message.find("mixPercent") != std::string::npos);
}

TEST_CASE("built-in default effects pass validation") {
    pluginrack::Scene scene = makeValidScene();
    REQUIRE(scene.channels.front().effects.size() == 2);
    REQUIRE(pluginrack::validate(scene).empty());
}

TEST_CASE("duplicate insert ids are rejected") {
    pluginrack::Scene scene = makeValidScene();
    scene.channels.front().inserts.push_back(pluginrack::InsertSlot{5, "a", "A"});
    scene.channels.front().inserts.push_back(pluginrack::InsertSlot{5, "b", "B"});
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::DuplicateInsertId);
    REQUIRE(errors[0].entityId == 1);
}

TEST_CASE("duplicate effect ids are rejected") {
    pluginrack::Scene scene = makeValidScene();
    pluginrack::DynamicsEffect duplicate = scene.channels.front().effects.front();
    scene.channels.front().effects.push_back(duplicate);
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::DuplicateEffectId);
    REQUIRE(errors[0].entityId == 1);
}

TEST_CASE("a chain that resolves to the strip's rows is accepted") {
    pluginrack::Scene scene = makeValidScene();
    scene.channels.front().inserts.push_back(pluginrack::InsertSlot{7, "a", "A"});
    scene.channels.front().chain = {{pluginrack::ChainItemType::Eq, 0},
                                    {pluginrack::ChainItemType::Dynamics, 1},
                                    {pluginrack::ChainItemType::Insert, 7}};
    REQUIRE(pluginrack::validate(scene).empty());
}

TEST_CASE("chain entries must resolve to the strip's own rows") {
    pluginrack::Scene scene = makeValidScene();
    pluginrack::Channel& channel = scene.channels.front();

    SECTION("unknown insert") {
        channel.chain = {{pluginrack::ChainItemType::Insert, 99}};
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].code == pluginrack::SceneErrorCode::UnknownChainItem);
        REQUIRE(errors[0].message.find("unknown insert 99") != std::string::npos);
    }
    SECTION("stale dynamics row is tolerated") {
        // Scenes written by earlier builds carry rows such as `Dynamics 2`
        // next to effects 1 and 3. The mixer skips them, so validation must not
        // refuse a real session over one.
        channel.effects = {{1, pluginrack::DynamicsEffectType::Compressor, true},
                           {3, pluginrack::DynamicsEffectType::Gate, true}};
        channel.chain = {{pluginrack::ChainItemType::Eq, 0},
                         {pluginrack::ChainItemType::Dynamics, 1},
                         {pluginrack::ChainItemType::Dynamics, 2}};
        REQUIRE(pluginrack::validate(scene).empty());
    }
    SECTION("Eq row with a non-zero id") {
        channel.chain = {{pluginrack::ChainItemType::Eq, 4}};
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].code == pluginrack::SceneErrorCode::UnknownChainItem);
    }
    SECTION("repeated row") {
        channel.chain = {{pluginrack::ChainItemType::Dynamics, 1},
                         {pluginrack::ChainItemType::Dynamics, 1}};
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].code == pluginrack::SceneErrorCode::DuplicateChainItem);
    }
}

TEST_CASE("output chain is validated the same way") {
    pluginrack::Scene scene = makeValidScene();
    scene.outputs.front().chain = {{pluginrack::ChainItemType::Insert, 42}};
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::UnknownChainItem);
    REQUIRE(errors[0].entityId == 1);
}

TEST_CASE("collection caps are enforced") {
    pluginrack::Scene scene = makeValidScene();

    SECTION("many insert slots are allowed") {
        // VST inserts are unbounded per strip; the floor is only the old
        // 5-slot UI cap, which a scene from an older build may exceed.
        for (std::uint32_t i = 1; i <= 8; ++i)
            scene.channels.front().inserts.push_back(
                pluginrack::InsertSlot{i, "p", "n"});
            scene.channels.front().chain.push_back(
                {pluginrack::ChainItemType::Insert, 1});
            scene.channels.front().chain.push_back(
                {pluginrack::ChainItemType::Insert, 2});
            scene.channels.front().chain.push_back(
                {pluginrack::ChainItemType::Insert, 3});
            scene.channels.front().chain.push_back(
                {pluginrack::ChainItemType::Insert, 4});
            scene.channels.front().chain.push_back(
                {pluginrack::ChainItemType::Insert, 5});
            scene.channels.front().chain.push_back(
                {pluginrack::ChainItemType::Insert, 6});
            scene.channels.front().chain.push_back(
                {pluginrack::ChainItemType::Insert, 7});
            scene.channels.front().chain.push_back(
                {pluginrack::ChainItemType::Insert, 8});
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors.empty());
    }
    SECTION("too many chain rows") {
        // The cap leaves room for every insert the engine allows (64) plus the
        // built-ins, so it takes 81 rows to trip it.
        for (std::uint32_t i = 0; i < 81; ++i)
            scene.channels.front().chain.push_back({pluginrack::ChainItemType::Insert, i + 1});
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
        REQUIRE(errors[0].message.find("chain") != std::string::npos);
    }
    SECTION("too many channels") {
        for (std::uint32_t i = 2; i <= 65; ++i) {
            pluginrack::Channel channel;
            channel.id = i;
            scene.channels.push_back(channel);
        }
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
        REQUIRE(errors[0].message.find("channels") != std::string::npos);
    }
    SECTION("too many outputs") {
        for (std::uint32_t i = 2; i <= 33; ++i) {
            pluginrack::Output output;
            output.id = i;
            scene.outputs.push_back(output);
        }
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
        REQUIRE(errors[0].message.find("outputs") != std::string::npos);
    }
}

TEST_CASE("the wasapi probe cap is bounded", "[scene][device]") {
    auto scene = makeValidScene();
    scene.audio = pluginrack::AudioSettings{};
    scene.audio->wasapiProbeCapEnabled = true;
    scene.audio->wasapiProbeCapChannels = 1;
    auto errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);

    scene.audio->wasapiProbeCapChannels = 65;
    errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);

    scene.audio->wasapiProbeCapChannels = 2;
    CHECK(pluginrack::validate(scene).empty());
    scene.audio->wasapiProbeCapChannels = 64;
    CHECK(pluginrack::validate(scene).empty());
}

TEST_CASE("watched devices must reference an existing strip") {
    pluginrack::Scene scene = makeValidScene();

    SECTION("unknown input strip") {
        scene.audio = pluginrack::AudioSettings{};
        scene.audio->watchedDevices.push_back({true, 99});
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].code == pluginrack::SceneErrorCode::UnknownWatchedDevice);
    }
    SECTION("unknown output strip") {
        scene.audio = pluginrack::AudioSettings{};
        scene.audio->watchedDevices.push_back({false, 99});
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].code == pluginrack::SceneErrorCode::UnknownWatchedDevice);
    }
    SECTION("strip listed twice") {
        scene.audio = pluginrack::AudioSettings{};
        scene.audio->watchedDevices.push_back({true, 1});
        scene.audio->watchedDevices.push_back({true, 1});
        const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].code == pluginrack::SceneErrorCode::UnknownWatchedDevice);
        REQUIRE(errors[0].message.find("listed twice") != std::string::npos);
    }
    SECTION("input and output strip may share the same id") {
        scene.audio = pluginrack::AudioSettings{};
        scene.audio->watchedDevices.push_back({true, 1});
        scene.audio->watchedDevices.push_back({false, 1});
        REQUIRE(pluginrack::validate(scene).empty());
    }
}

TEST_CASE("de-esser envelope times are range-checked") {
    pluginrack::Scene scene = makeValidScene();
    pluginrack::DeEsserEffect deEsser;
    deEsser.releaseMs = 1.0e9f;
    scene.channels.front().deEsserEffects.push_back(deEsser);
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    REQUIRE(errors.front().code == pluginrack::SceneErrorCode::ValueOutOfRange);
}

TEST_CASE("multiband band gains are range-checked") {
    pluginrack::Scene scene = makeValidScene();
    auto effect = pluginrack::makeDefaultMultibandEffect();
    effect.bands.front().makeupDb = 100.0f;
    scene.channels.front().multibandEffects.push_back(effect);
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    REQUIRE(errors.front().code == pluginrack::SceneErrorCode::ValueOutOfRange);
}

TEST_CASE("multiband bands need exactly one fewer crossover") {
    pluginrack::Scene scene = makeValidScene();
    auto effect = pluginrack::makeDefaultMultibandEffect();
    effect.crossovers.clear();
    scene.channels.front().multibandEffects.push_back(effect);
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    REQUIRE(errors.front().code == pluginrack::SceneErrorCode::ValueOutOfRange);
}

TEST_CASE("duplicate bus id is rejected") {
    pluginrack::Scene scene = makeValidScene();
    pluginrack::Bus bus;
    bus.id = 4;
    bus.name = "one";
    scene.buses.push_back(bus);
    bus.name = "two";
    scene.buses.push_back(bus);
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    bool found = false;
    for (const auto& error : errors) {
        if (error.code == pluginrack::SceneErrorCode::DuplicateBusId) found = true;
    }
    REQUIRE(found);
}

TEST_CASE("bus send must name a real channel and bus") {
    pluginrack::Scene scene = makeValidScene();
    pluginrack::Bus bus;
    bus.id = 4;
    scene.buses.push_back(bus);
    pluginrack::BusSend send;
    send.channelId = 99;
    send.busId = 4;
    scene.busSends.push_back(send);
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    REQUIRE(errors.front().code == pluginrack::SceneErrorCode::UnknownChannelInSend);
}

TEST_CASE("a tap may name the eq row, which is id zero") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = pluginrack::kSchemaVersion;
    scene.channels.front().eqEnabled = true;
    scene.channels.front().chain = {{pluginrack::ChainItemType::Eq, 0}};
    pluginrack::Tap tap;
    tap.kind = pluginrack::Tap::Kind::Item;
    tap.itemId = 0; // the eq row, and the patch view offers it as a source
    scene.routes.front().tap = tap;
    REQUIRE(pluginrack::validate(scene).empty());

    // Zero is only legal because a row carries it: without the eq row the tap
    // names nothing, and the scene must be rejected rather than open silent.
    scene.channels.front().chain.clear();
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    REQUIRE(errors.front().code == pluginrack::SceneErrorCode::UnknownTapItem);
}

TEST_CASE("a typed item tap must name a row of that type") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = pluginrack::kSchemaVersion;
    scene.channels.front().chain = {{pluginrack::ChainItemType::Dynamics, 1}};
    pluginrack::Tap tap;
    tap.kind = pluginrack::Tap::Kind::Item;
    tap.itemId = 1;

    tap.hasItemType = true;
    tap.itemType = pluginrack::ChainItemType::Dynamics;
    scene.routes.front().tap = tap;
    REQUIRE(pluginrack::validate(scene).empty());

    // The same id from another family names no row, so the scene is rejected
    // instead of silently resolving to the Gate row.
    tap.itemType = pluginrack::ChainItemType::FeedbackSuppressor;
    scene.routes.front().tap = tap;
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    REQUIRE(errors.front().code == pluginrack::SceneErrorCode::UnknownTapItem);

    // An id-only tap keeps the older rule and only needs some row with the id.
    tap.hasItemType = false;
    scene.routes.front().tap = tap;
    REQUIRE(pluginrack::validate(scene).empty());
}

TEST_CASE("bus count over the cap is rejected") {
    pluginrack::Scene scene = makeValidScene();
    for (std::size_t i = 0; i <= pluginrack::kMaxBuses; ++i) {
        pluginrack::Bus bus;
        bus.id = static_cast<std::uint32_t>(100 + i);
        scene.buses.push_back(bus);
    }
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    REQUIRE(errors.front().code == pluginrack::SceneErrorCode::ValueOutOfRange);
}

TEST_CASE("insert side-chain to a missing strip is rejected") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = 11;
    pluginrack::InsertSlot slot;
    slot.id = 5;
    slot.sideChainSource = pluginrack::SideChainSource::Channel;
    slot.sideChainSourceId = 99;
    scene.channels.front().inserts.push_back(slot);
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    REQUIRE(errors.front().code == pluginrack::SceneErrorCode::UnknownSideChainSourceId);
}

TEST_CASE("insert cannot side-chain from its own channel") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = 11;
    pluginrack::InsertSlot slot;
    slot.id = 5;
    slot.sideChainSource = pluginrack::SideChainSource::Channel;
    slot.sideChainSourceId = 1;
    scene.channels.front().inserts.push_back(slot);
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    bool feedback = false;
    for (const auto& error : errors) {
        if (error.message.find("feedback loop") != std::string::npos) feedback = true;
    }
    REQUIRE(feedback);
}

TEST_CASE("channel side-chaining a same-id bus is not self-feedback") {
    // Ids are only unique per strip kind: channel 1 keying off bus 1 is a
    // legal routing, but the shared numeric id used to read as a feedback
    // loop and the scene was rejected.
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = pluginrack::kSchemaVersion;
    pluginrack::Bus bus;
    bus.id = 1;
    bus.name = "keys";
    scene.buses.push_back(bus);
    pluginrack::InsertSlot slot;
    slot.id = 5;
    slot.sideChainSource = pluginrack::SideChainSource::Bus;
    slot.sideChainSourceId = 1;
    scene.channels.front().inserts.push_back(slot);
    REQUIRE(pluginrack::validate(scene).empty());
}

TEST_CASE("bus insert cannot side-chain from its own bus") {
    pluginrack::Scene scene = makeValidScene();
    scene.schemaVersion = pluginrack::kSchemaVersion;
    pluginrack::Bus bus;
    bus.id = 1;
    bus.name = "keys";
    pluginrack::InsertSlot slot;
    slot.id = 5;
    slot.sideChainSource = pluginrack::SideChainSource::Bus;
    slot.sideChainSourceId = 1;
    bus.inserts.push_back(slot);
    scene.buses.push_back(bus);
    const auto errors = pluginrack::validate(scene);
    REQUIRE_FALSE(errors.empty());
    bool feedback = false;
    for (const auto& error : errors) {
        if (error.message.find("feedback loop") != std::string::npos) feedback = true;
    }
    REQUIRE(feedback);
}

TEST_CASE("output firstChannel starts the pair on any device channel", "[scene][device]") {
    pluginrack::Scene scene = makeValidScene();
    scene.outputs.front().firstChannel = 0;
    REQUIRE(pluginrack::validate(scene).empty());
    scene.outputs.front().firstChannel = 2;
    REQUIRE(pluginrack::validate(scene).empty());
    // Odd starts are valid: a device numbered 1, 2, 3, ... can host a pair on
    // channels 3/4.
    scene.outputs.front().firstChannel = 1;
    REQUIRE(pluginrack::validate(scene).empty());
    scene.outputs.front().firstChannel = 62;
    REQUIRE(pluginrack::validate(scene).empty());

    scene.outputs.front().firstChannel = -2;
    auto errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].message.find("firstChannel") != std::string::npos);

    scene.outputs.front().firstChannel = 64;
    errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].message.find("firstChannel") != std::string::npos);
}

TEST_CASE("spectral denoiser fft size must be a supported size") {
    pluginrack::Scene scene = makeValidScene();
    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect(1);
    scene.channels.front().spectralDenoiserEffects = {effect};

    for (const int fftSize : pluginrack::kSpectralDenoiserFftSizes) {
        effect.fftSize = fftSize;
        scene.channels.front().spectralDenoiserEffects = {effect};
        REQUIRE(pluginrack::validate(scene).empty());
    }

    effect.fftSize = 1000;
    scene.channels.front().spectralDenoiserEffects = {effect};
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::ValueOutOfRange);
    REQUIRE(errors[0].entityId == 1);
    REQUIRE(errors[0].message.find("fftSize") != std::string::npos);
}
