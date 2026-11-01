#include "pluginrack/SceneValidator.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace pluginrack {

namespace {

constexpr float kMinGainDb = -60.0f;
constexpr float kMaxGainDb = 12.0f;
constexpr float kMinTrimDb = -12.0f;
constexpr float kMaxTrimDb = 12.0f;
constexpr float kMinPan = -1.0f;
constexpr float kMaxPan = 1.0f;
constexpr float kMinEqFreqHz = 10.0f;
constexpr float kMaxEqFreqHz = 22050.0f;
constexpr float kMinEqGainDb = -15.0f;
constexpr float kMaxEqGainDb = 15.0f;
constexpr float kMinEqQ = 0.1f;
constexpr float kMaxEqQ = 20.0f;
constexpr float kMinEqDelayMs = 0.0f;
constexpr float kMaxEqDelayMs = 1000.0f;
constexpr std::size_t kMaxEqBands = 6;
constexpr std::size_t kMaxEffects = 4;

bool isSupportedSampleRate(double sampleRate) {
    return sampleRate == 44100.0 || sampleRate == 48000.0 || sampleRate == 88200.0 ||
           sampleRate == 96000.0;
}

void validateEffects(const char* kind, std::uint32_t id,
                     const std::vector<DynamicsEffect>& effects,
                     std::vector<SceneError>& errors) {
    if (effects.size() > kMaxEffects) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                          std::string(kind) + " " + std::to_string(id) +
                              " effects has more than 4 slots"});
    }
    for (const auto& effect : effects) {
        if (effect.id == 0 || !std::isfinite(effect.thresholdDb) ||
            !std::isfinite(effect.ratio) || effect.ratio < 1.0f ||
            !std::isfinite(effect.attackMs) || effect.attackMs <= 0.0f ||
            !std::isfinite(effect.releaseMs) || effect.releaseMs <= 0.0f) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                              std::string(kind) + " " + std::to_string(id) +
                                  " has invalid dynamics effect parameters"});
        }
    }
}

void validateEqBands(const char* kind, std::uint32_t id, const std::vector<EqBand>& bands,
                     std::vector<SceneError>& errors) {
    if (bands.size() > kMaxEqBands) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                          std::string(kind) + " " + std::to_string(id) + " eqBands has " +
                              std::to_string(bands.size()) + " bands, expected at most 6"});
    }
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const EqBand& band = bands[i];
        const std::string where =
            std::string(kind) + " " + std::to_string(id) + " eqBands[" + std::to_string(i) + "]";
        if (!std::isfinite(band.freqHz) || band.freqHz < kMinEqFreqHz ||
            band.freqHz > kMaxEqFreqHz) {
            errors.push_back(
                {SceneErrorCode::ValueOutOfRange, id, where + " freqHz out of range [10, 22050]"});
        }
        if (!std::isfinite(band.gainDb) || band.gainDb < kMinEqGainDb ||
            band.gainDb > kMaxEqGainDb) {
            errors.push_back(
                {SceneErrorCode::ValueOutOfRange, id, where + " gainDb out of range [-15, 15]"});
        }
        if (!std::isfinite(band.q) || band.q < kMinEqQ || band.q > kMaxEqQ) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                              where + " q out of range [0.1, 20]"});
        }
    }
}

void validateEqDelay(const char* kind, std::uint32_t id, float delayMs,
                     std::vector<SceneError>& errors) {
    if (!std::isfinite(delayMs) || delayMs < kMinEqDelayMs || delayMs > kMaxEqDelayMs) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                          std::string(kind) + " " + std::to_string(id) +
                              " eqDelayMs out of range [0, 1000]"});
    }
}

std::string describeMapping(const std::vector<int>& mapping) {
    std::string text = "[";
    for (std::size_t i = 0; i < mapping.size(); ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += std::to_string(mapping[i]);
    }
    text += "]";
    return text;
}

} // namespace

std::vector<SceneError> validate(const Scene& scene) {
    std::vector<SceneError> errors;

    if (scene.schemaVersion < 1 || scene.schemaVersion > 4) {
        errors.push_back({SceneErrorCode::UnsupportedSchemaVersion, 0,
                          "unsupported schemaVersion " + std::to_string(scene.schemaVersion) +
                              "; expected 1, 2 or 3"});
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
        if (!std::isfinite(channel.trimDb) || channel.trimDb < kMinTrimDb ||
            channel.trimDb > kMaxTrimDb) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, channel.id,
                              "channel " + std::to_string(channel.id) +
                                  " trimDb out of range [-12, 12]"});
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
        if (channel.layout == ChannelLayout::Mono) {
            if (channel.inputChannels.size() != 1 || channel.inputChannels[0] < 0) {
                errors.push_back({SceneErrorCode::ValueOutOfRange, channel.id,
                                  "channel " + std::to_string(channel.id) + " inputChannels " +
                                      describeMapping(channel.inputChannels) +
                                      " must contain exactly one non-negative index"});
            }
        } else if (channel.inputChannels.size() != 2 || channel.inputChannels[0] < 0 ||
                   channel.inputChannels[1] < 0 ||
                   channel.inputChannels[0] == channel.inputChannels[1]) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, channel.id,
                              "channel " + std::to_string(channel.id) + " inputChannels " +
                                  describeMapping(channel.inputChannels) +
                                  " must contain exactly two distinct non-negative indexes"});
        }
        validateEqBands("channel", channel.id, channel.eqBands, errors);
        validateEqDelay("channel", channel.id, channel.eqDelayMs, errors);
        validateEffects("channel", channel.id, channel.effects, errors);
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
        if (!std::isfinite(output.gainDb) || output.gainDb < kMinGainDb ||
            output.gainDb > kMaxGainDb) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, output.id,
                              "output " + std::to_string(output.id) +
                                  " gainDb out of range [-60, 12]"});
        }
        for (const InsertSlot& slot : output.inserts) {
            if (slot.id == 0) {
                errors.push_back({SceneErrorCode::ValueOutOfRange, output.id,
                                  "output " + std::to_string(output.id) +
                                      " insert slot id must not be zero"});
            }
        }
        validateEqBands("output", output.id, output.eqBands, errors);
        validateEqDelay("output", output.id, output.eqDelayMs, errors);
        validateEffects("output", output.id, output.effects, errors);
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
