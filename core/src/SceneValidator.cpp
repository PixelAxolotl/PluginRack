#include "pluginrack/SceneValidator.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace pluginrack {

namespace {

constexpr float kMinGainDb = -60.0f;
constexpr float kMaxGainDb = 12.0f;
constexpr float kMinPan = -1.0f;
constexpr float kMaxPan = 1.0f;

bool isSupportedSampleRate(double sampleRate) {
    return sampleRate == 44100.0 || sampleRate == 48000.0 || sampleRate == 88200.0 ||
           sampleRate == 96000.0;
}

} // namespace

std::vector<SceneError> validate(const Scene& scene) {
    std::vector<SceneError> errors;

    if (scene.schemaVersion != 1) {
        errors.push_back({SceneErrorCode::UnsupportedSchemaVersion, 0,
                          "unsupported schemaVersion " + std::to_string(scene.schemaVersion) +
                              "; expected 1"});
    }
    if (scene.channels.empty()) {
        errors.push_back({SceneErrorCode::NoChannels, 0, "scene has no channels"});
    }
    if (scene.outputs.empty()) {
        errors.push_back({SceneErrorCode::NoOutputs, 0, "scene has no outputs"});
    }

    std::unordered_set<std::uint32_t> channelIds;
    channelIds.reserve(scene.channels.size() * 2 + 1);
    for (const Channel& channel : scene.channels) {
        if (channel.id == 0) {
            errors.push_back(
                {SceneErrorCode::ValueOutOfRange, 0, "channel id must not be zero"});
        }
        if (!channelIds.insert(channel.id).second) {
            errors.push_back({SceneErrorCode::DuplicateChannelId, channel.id,
                              "duplicate channel id " + std::to_string(channel.id)});
        }
        if (!std::isfinite(channel.gainDb) || channel.gainDb < kMinGainDb ||
            channel.gainDb > kMaxGainDb) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, channel.id,
                              "channel " + std::to_string(channel.id) +
                                  " gainDb out of range [-60, 12]"});
        }
        if (!std::isfinite(channel.pan) || channel.pan < kMinPan || channel.pan > kMaxPan) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, channel.id,
                              "channel " + std::to_string(channel.id) +
                                  " pan out of range [-1, 1]"});
        }
        for (const InsertSlot& slot : channel.inserts) {
            if (slot.id == 0) {
                errors.push_back({SceneErrorCode::ValueOutOfRange, channel.id,
                                  "channel " + std::to_string(channel.id) +
                                      " insert slot id must not be zero"});
            }
        }
    }

    std::unordered_set<std::uint32_t> outputIds;
    outputIds.reserve(scene.outputs.size() * 2 + 1);
    for (const Output& output : scene.outputs) {
        if (output.id == 0) {
            errors.push_back(
                {SceneErrorCode::ValueOutOfRange, 0, "output id must not be zero"});
        }
        if (!outputIds.insert(output.id).second) {
            errors.push_back({SceneErrorCode::DuplicateOutputId, output.id,
                              "duplicate output id " + std::to_string(output.id)});
        }
    }

    std::unordered_set<std::uint64_t> routePairs;
    routePairs.reserve(scene.routes.size() * 2 + 1);
    for (const Route& route : scene.routes) {
        if (route.channelId == 0) {
            errors.push_back(
                {SceneErrorCode::ValueOutOfRange, 0, "route channelId must not be zero"});
        }
        if (route.outputId == 0) {
            errors.push_back(
                {SceneErrorCode::ValueOutOfRange, 0, "route outputId must not be zero"});
        }
        const std::uint64_t pair =
            (static_cast<std::uint64_t>(route.channelId) << 32) | route.outputId;
        if (!routePairs.insert(pair).second) {
            errors.push_back({SceneErrorCode::DuplicateRoute, route.channelId,
                              "duplicate route (" + std::to_string(route.channelId) + ", " +
                                  std::to_string(route.outputId) + ")"});
        }
        if (route.channelId != 0 && !channelIds.contains(route.channelId)) {
            errors.push_back({SceneErrorCode::UnknownChannelInRoute, route.channelId,
                              "route references unknown channel " +
                                  std::to_string(route.channelId)});
        }
        if (route.outputId != 0 && !outputIds.contains(route.outputId)) {
            errors.push_back({SceneErrorCode::UnknownOutputInRoute, route.outputId,
                              "route references unknown output " +
                                  std::to_string(route.outputId)});
        }
        if (!std::isfinite(route.sendGainDb) || route.sendGainDb < kMinGainDb ||
            route.sendGainDb > kMaxGainDb) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, route.channelId,
                              "route (" + std::to_string(route.channelId) + ", " +
                                  std::to_string(route.outputId) +
                                  ") sendGainDb out of range [-60, 12]"});
        }
    }

    if (!isSupportedSampleRate(scene.sampleRate)) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, 0,
                          "sampleRate " + std::to_string(scene.sampleRate) +
                              " is not one of 44100, 48000, 88200, 96000"});
    }

    return errors;
}

} // namespace pluginrack
