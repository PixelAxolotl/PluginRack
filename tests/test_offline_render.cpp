#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pluginrack/DeviceRegistry.h"
#include "pluginrack/OfflineRenderer.h"
#include "pluginrack/SceneAvailability.h"
#include "pluginrack/WavWriter.h"

namespace {

class AllPresent : public pluginrack::DeviceRegistry {
public:
    bool hasInput(const std::string&) const override { return true; }
    bool hasOutput(const std::string&) const override { return true; }
};

pluginrack::Scene makeScene() {
    pluginrack::Scene scene;
    scene.schemaVersion = 1;
    scene.name = "offline";
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;
    return scene;
}

void addChannel(pluginrack::Scene& scene, std::uint32_t id) {
    pluginrack::Channel channel;
    channel.id = id;
    channel.name = "ch" + std::to_string(id);
    channel.layout = pluginrack::ChannelLayout::Mono;
    channel.gainDb = 0.0f;
    channel.pan = 0.0f;
    scene.channels.push_back(channel);
}

void addOutput(pluginrack::Scene& scene, std::uint32_t id) {
    pluginrack::Output output;
    output.id = id;
    output.name = "out" + std::to_string(id);
    scene.outputs.push_back(output);
}

void addRoute(pluginrack::Scene& scene, std::uint32_t channelId, std::uint32_t outputId) {
    pluginrack::Route route;
    route.channelId = channelId;
    route.outputId = outputId;
    route.sendGainDb = 0.0f;
    scene.routes.push_back(route);
}

pluginrack::SceneAvailability allAvailable(const pluginrack::Scene& scene) {
    const AllPresent registry;
    return pluginrack::resolveAvailability(scene, registry);
}

} // namespace

TEST_CASE("sine render has the expected amplitude and frequency") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addRoute(scene, 1, 1);

    constexpr int kFrames = 48000;
    const std::vector<std::vector<float>> result =
        pluginrack::renderSine(scene, allAvailable(scene), 440.0f, 1.0f, kFrames);
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].size() == static_cast<std::size_t>(kFrames) * 2);

    float peak = 0.0f;
    for (const float sample : result[0]) {
        const float abs = std::fabs(sample);
        if (abs > peak) {
            peak = abs;
        }
    }
    REQUIRE(peak == Catch::Approx(0.70710678f).epsilon(0.01f));

    int lastSign = 0;
    int signChanges = 0;
    for (int i = 0; i < kFrames; ++i) {
        const float sample = result[0][static_cast<std::size_t>(i) * 2];
        const int sign = sample > 0.0f ? 1 : (sample < 0.0f ? -1 : 0);
        if (sign == 0) {
            continue;
        }
        if (lastSign != 0 && sign != lastSign) {
            ++signChanges;
        }
        lastSign = sign;
    }
    REQUIRE(std::abs(signChanges - 880) <= 2);
}

TEST_CASE("render returns one buffer per output") {
    pluginrack::Scene scene = makeScene();
    addChannel(scene, 1);
    addOutput(scene, 1);
    addOutput(scene, 2);
    addRoute(scene, 1, 1);
    addRoute(scene, 1, 2);

    constexpr int kFrames = 512;
    const std::vector<std::vector<float>> result =
        pluginrack::renderSine(scene, allAvailable(scene), 440.0f, 1.0f, kFrames);
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].size() == static_cast<std::size_t>(kFrames) * 2);
    REQUIRE(result[1].size() == static_cast<std::size_t>(kFrames) * 2);
}

TEST_CASE("wav header is 16-bit stereo at 48 kHz") {
    const std::vector<float> samples(100 * 2, 0.0f);
    const std::string path =
        (std::filesystem::temp_directory_path() / "pluginrack-wav-test.wav").string();
    REQUIRE(pluginrack::writeWav(path, samples, 2, 48000));

    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    char header[44] = {};
    file.read(header, sizeof(header));
    REQUIRE(file.gcount() == sizeof(header));
    REQUIRE(std::string(header, 4) == "RIFF");
    REQUIRE(std::string(header + 8, 4) == "WAVE");
    const auto u16 = [&](int offset) {
        return static_cast<unsigned int>(static_cast<unsigned char>(header[offset])) |
               (static_cast<unsigned int>(static_cast<unsigned char>(header[offset + 1]))
                << 8);
    };
    const auto u32 = [&](int offset) {
        return u16(offset) | (u16(offset + 2) << 16);
    };
    REQUIRE(u16(22) == 2);
    REQUIRE(u32(24) == 48000);
    REQUIRE(u16(34) == 16);
    file.seekg(0, std::ios::end);
    REQUIRE(file.tellg() == 44 + 100 * 4);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
