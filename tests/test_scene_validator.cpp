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
    scene.schemaVersion = 2;
    const std::vector<pluginrack::SceneError> errors = pluginrack::validate(scene);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].code == pluginrack::SceneErrorCode::UnsupportedSchemaVersion);
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
