#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

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
    scene.schemaVersion = 5;
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::UnsupportedSchemaVersion);
    // The message must name the range the validator actually accepts.
    REQUIRE(errors[0].message.find("expected 1 to 4") != std::string::npos);
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
