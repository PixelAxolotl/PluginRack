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
constexpr std::size_t kMaxEqBands = 7;
constexpr std::size_t kMaxEffects = 4;
constexpr std::size_t kMaxInserts = 5;
constexpr std::size_t kMaxChainItems = 16;
constexpr std::size_t kMaxChannels = 64;
constexpr std::size_t kMaxOutputs = 32;
constexpr std::size_t kMaxRoutes = 1024;
constexpr std::size_t kMaxWatchedDevices = 64;
// Buffer sizes the device layer negotiates; anything outside this is a sign of
// a hand-edited or foreign file, not a real session.
constexpr int kMinBufferFrames = 64;
constexpr int kMaxBufferFrames = 8192;
constexpr float kMinDynamicsMakeupDb = -24.0f;
constexpr float kMaxDynamicsMakeupDb = 24.0f;
constexpr float kMinDynamicsInputGainDb = -24.0f;
constexpr float kMaxDynamicsInputGainDb = 24.0f;
constexpr float kMinDynamicsKneeDb = 0.0f;
constexpr float kMaxDynamicsKneeDb = 24.0f;
// The ranges the editor's dynamics knobs expose, so every scene this build
// writes passes and anything else is flagged.
constexpr float kMinDynamicsThresholdDb = -60.0f;
constexpr float kMaxDynamicsThresholdDb = 0.0f;
constexpr float kMaxDynamicsRatio = 80.0f;
constexpr float kMaxDynamicsAttackMs = 200.0f;
constexpr float kMaxDynamicsReleaseMs = 1000.0f;
// EQWindow's RTA display controls.
constexpr float kMinEqRtaOffsetDb = -60.0f;
constexpr float kMaxEqRtaOffsetDb = 60.0f;
constexpr int kMaxEqRtaWeighting = 2;

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
    std::unordered_set<std::uint32_t> effectIds;
    effectIds.reserve(effects.size() * 2 + 1);
    for (const auto& effect : effects) {
        if (effect.id == 0 || !std::isfinite(effect.thresholdDb) ||
            effect.thresholdDb < kMinDynamicsThresholdDb ||
            effect.thresholdDb > kMaxDynamicsThresholdDb ||
            !std::isfinite(effect.ratio) || effect.ratio < 1.0f ||
            effect.ratio > kMaxDynamicsRatio ||
            !std::isfinite(effect.attackMs) || effect.attackMs <= 0.0f ||
            effect.attackMs > kMaxDynamicsAttackMs ||
            !std::isfinite(effect.releaseMs) || effect.releaseMs <= 0.0f ||
            effect.releaseMs > kMaxDynamicsReleaseMs ||
            !std::isfinite(effect.makeupDb) ||
            effect.makeupDb < kMinDynamicsMakeupDb ||
            effect.makeupDb > kMaxDynamicsMakeupDb ||
            !std::isfinite(effect.inputGainDb) ||
            effect.inputGainDb < kMinDynamicsInputGainDb ||
            effect.inputGainDb > kMaxDynamicsInputGainDb ||
            !std::isfinite(effect.kneeDb) ||
            effect.kneeDb < kMinDynamicsKneeDb ||
            effect.kneeDb > kMaxDynamicsKneeDb) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                                  std::string(kind) + " " + std::to_string(id) +
                                  " has invalid dynamics effect parameters"});
        }
        // The chain refers to effects by id, so a duplicate makes the lookup
        // ambiguous and the strip would run the wrong row.
        if (effect.id != 0 && !effectIds.insert(effect.id).second) {
            errors.push_back({SceneErrorCode::DuplicateEffectId, id,
                              std::string(kind) + " " + std::to_string(id) +
                                  " has duplicate effect id " + std::to_string(effect.id)});
        }
    }
}

void validateInserts(const char* kind, std::uint32_t id,
                     const std::vector<InsertSlot>& inserts,
                     std::vector<SceneError>& errors) {
    if (inserts.size() > kMaxInserts) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                          std::string(kind) + " " + std::to_string(id) + " has " +
                              std::to_string(inserts.size()) + " inserts, expected at most 5"});
    }
    std::unordered_set<std::uint32_t> insertIds;
    insertIds.reserve(inserts.size() * 2 + 1);
    for (const InsertSlot& slot : inserts) {
        if (slot.id == 0) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                              std::string(kind) + " " + std::to_string(id) +
                                  " insert slot id must not be zero"});
        } else if (!insertIds.insert(slot.id).second) {
            errors.push_back({SceneErrorCode::DuplicateInsertId, id,
                              std::string(kind) + " " + std::to_string(id) +
                                  " has duplicate insert id " + std::to_string(slot.id)});
        }
    }
}

// Every chain row must point at a row the strip actually holds. Insert rows are
// maintained atomically with the inserts themselves, so a dangling one is a
// corrupt file; the render path would skip it silently and the scene would open
// looking healthy while that row never runs.
void validateChain(const char* kind, std::uint32_t id, const std::vector<ChainItem>& chain,
                   const std::vector<InsertSlot>& inserts, std::vector<SceneError>& errors) {
    if (chain.size() > kMaxChainItems) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                          std::string(kind) + " " + std::to_string(id) + " chain has " +
                              std::to_string(chain.size()) + " items, expected at most 16"});
    }
    std::unordered_set<std::uint32_t> insertIds;
    for (const InsertSlot& slot : inserts) insertIds.insert(slot.id);

    std::unordered_set<std::uint64_t> seen;
    seen.reserve(chain.size() * 2 + 1);
    for (const ChainItem& item : chain) {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(static_cast<int>(item.type)) << 32) | item.id;
        if (!seen.insert(key).second) {
            errors.push_back({SceneErrorCode::DuplicateChainItem, id,
                              std::string(kind) + " " + std::to_string(id) +
                                  " chain repeats item id " + std::to_string(item.id)});
            continue;
        }
        switch (item.type) {
            case ChainItemType::Eq:
                if (item.id != 0) {
                    errors.push_back({SceneErrorCode::UnknownChainItem, id,
                                      std::string(kind) + " " + std::to_string(id) +
                                          " chain has an Eq item with id " +
                                          std::to_string(item.id) + ", expected 0"});
                }
                break;
            case ChainItemType::Dynamics:
                // Deliberately not checked against the strip's effects: scenes
                // written by earlier builds carry rows such as a stale
                // `Dynamics 2` next to effects 1 and 3, and the mixer tolerates
                // them (an unresolvable row simply does not run). Rejecting
                // those files would refuse to open real sessions.
                break;
            case ChainItemType::Insert:
                if (!insertIds.contains(item.id)) {
                    errors.push_back({SceneErrorCode::UnknownChainItem, id,
                                      std::string(kind) + " " + std::to_string(id) +
                                          " chain references unknown insert " +
                                          std::to_string(item.id)});
                }
                break;
        }
    }
}

void validateRtaSettings(const char* kind, std::uint32_t id, float offsetDb, int weighting,
                         std::vector<SceneError>& errors) {
    if (!std::isfinite(offsetDb) || offsetDb < kMinEqRtaOffsetDb || offsetDb > kMaxEqRtaOffsetDb) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                          std::string(kind) + " " + std::to_string(id) +
                              " eqRtaOffsetDb out of range [-60, 60]"});
    }
    if (weighting < 0 || weighting > kMaxEqRtaWeighting) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                          std::string(kind) + " " + std::to_string(id) +
                              " eqRtaWeighting must be 0 (Z), 1 (A) or 2 (C)"});
    }
}

void validateEqBands(const char* kind, std::uint32_t id, const std::vector<EqBand>& bands,
                     std::vector<SceneError>& errors) {
    if (bands.size() > kMaxEqBands) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, id,
                          std::string(kind) + " " + std::to_string(id) + " eqBands has " +
                              std::to_string(bands.size()) + " bands, expected at most 7"});
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
                              "; expected 1 to 4"});
    }
    if (scene.channels.empty()) {
        errors.push_back({SceneErrorCode::NoChannels, 0, "scene has no channels"});
    }
    if (scene.outputs.empty()) {
        errors.push_back({SceneErrorCode::NoOutputs, 0, "scene has no outputs"});
    }
    // Caps keep a hand-edited file from reserving an unbounded number of
    // strips and routes in prepare().
    if (scene.channels.size() > kMaxChannels) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, 0,
                          "scene has " + std::to_string(scene.channels.size()) +
                              " channels, expected at most 64"});
    }
    if (scene.outputs.size() > kMaxOutputs) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, 0,
                          "scene has " + std::to_string(scene.outputs.size()) +
                              " outputs, expected at most 32"});
    }
    if (scene.routes.size() > kMaxRoutes) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, 0,
                          "scene has " + std::to_string(scene.routes.size()) +
                              " routes, expected at most 1024"});
    }
    if (scene.bufferFrames < kMinBufferFrames || scene.bufferFrames > kMaxBufferFrames) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, 0,
                          "bufferFrames " + std::to_string(scene.bufferFrames) +
                              " is not within [64, 8192]"});
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
        validateInserts("channel", channel.id, channel.inserts, errors);
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
        validateChain("channel", channel.id, channel.chain, channel.inserts, errors);
        validateRtaSettings("channel", channel.id, channel.eqRtaOffsetDb, channel.eqRtaWeighting,
                            errors);
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
        validateInserts("output", output.id, output.inserts, errors);
        validateEqBands("output", output.id, output.eqBands, errors);
        validateEqDelay("output", output.id, output.eqDelayMs, errors);
        validateEffects("output", output.id, output.effects, errors);
        validateChain("output", output.id, output.chain, output.inserts, errors);
        validateRtaSettings("output", output.id, output.eqRtaOffsetDb, output.eqRtaWeighting,
                            errors);
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
        if (route.sendGainDbR.has_value() &&
            (!std::isfinite(*route.sendGainDbR) || *route.sendGainDbR < kMinGainDb ||
             *route.sendGainDbR > kMaxGainDb)) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, route.channelId,
                              "route (" + std::to_string(route.channelId) + ", " +
                                  std::to_string(route.outputId) +
                                  ") sendGainDbR out of range [-60, 12]"});
        }
    }

    if (!isSupportedSampleRate(scene.sampleRate)) {
        errors.push_back({SceneErrorCode::ValueOutOfRange, 0,
                          "sampleRate " + std::to_string(scene.sampleRate) +
                              " is not one of 44100, 48000, 88200, 96000"});
    }

    if (scene.audio.has_value()) {
        const auto& watched = scene.audio->watchedDevices;
        if (watched.size() > kMaxWatchedDevices) {
            errors.push_back({SceneErrorCode::ValueOutOfRange, 0,
                              "scene watches " + std::to_string(watched.size()) +
                                  " devices, expected at most 64"});
        }
        std::unordered_set<std::uint64_t> watchedKeys;
        watchedKeys.reserve(watched.size() * 2 + 1);
        for (const WatchedDevice& device : watched) {
            const bool known = device.isInput ? channelIds.contains(device.stripId)
                                              : outputIds.contains(device.stripId);
            if (device.stripId == 0 || !known) {
                errors.push_back({SceneErrorCode::UnknownWatchedDevice, device.stripId,
                                  std::string("watched ") +
                                      (device.isInput ? "input" : "output") + " strip " +
                                      std::to_string(device.stripId) + " does not exist"});
                continue;
            }
            const std::uint64_t key =
                (device.isInput ? 1ull : 0ull) << 32 | static_cast<std::uint64_t>(device.stripId);
            if (!watchedKeys.insert(key).second) {
                errors.push_back({SceneErrorCode::UnknownWatchedDevice, device.stripId,
                                  std::string("watched ") +
                                      (device.isInput ? "input" : "output") + " strip " +
                                      std::to_string(device.stripId) + " is listed twice"});
            }
        }
    }

    return errors;
}

} // namespace pluginrack
