#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pluginrack/EqEngine.h"
#include "pluginrack/DynamicsEffect.h"
#include "pluginrack/Types.h"

namespace pluginrack {

struct InsertSlot {
    std::uint32_t id = 0;
    std::string pluginPath;
    std::string pluginName;
    bool bypassed = false;
    // Opaque base64 blob of the plugin's own state (parameters, programs,
    // samples). The core engine never interprets it; the host layer captures
    // it on save and feeds it back to the plugin when the slot is rebuilt.
    // Empty means the plugin keeps its defaults.
    std::string pluginState;

    bool operator==(const InsertSlot&) const = default;
};

struct Channel {
    std::uint32_t id = 0;
    std::string name;
    SourceKind sourceKind = SourceKind::Hardware;
    std::string sourceDeviceId;
    ChannelLayout layout = ChannelLayout::Mono;
    std::vector<int> inputChannels{0};
    float gainDb = 0.0f;
    float trimDb = 0.0f;
    float pan = 0.0f;
    bool muted = false;
    bool soloed = false;
    std::vector<InsertSlot> inserts;
    std::vector<DynamicsEffect> effects = defaultDynamicsEffects();
    bool eqEnabled = false;
    std::vector<EqBand> eqBands = defaultSixBands();
    // Post-EQ delay in milliseconds, 0..EqEngine::kMaxDelayMs; 0 keeps the
    // chain undelayed. Like the bands it only runs while eqEnabled.
    float eqDelayMs = 0.0f;

    bool operator==(const Channel&) const = default;
};

struct Output {
    std::uint32_t id = 0;
    std::string name;
    OutputKind kind = OutputKind::Physical;
    std::string deviceId;
    ChannelLayout layout = ChannelLayout::Stereo;
    bool flipped = false;
    float gainDb = 0.0f;
    bool muted = false;
    std::vector<InsertSlot> inserts;
    std::vector<DynamicsEffect> effects = defaultDynamicsEffects();
    bool eqEnabled = false;
    std::vector<EqBand> eqBands = defaultSixBands();
    // Post-EQ delay in milliseconds, 0..EqEngine::kMaxDelayMs; 0 keeps the
    // chain undelayed. Like the bands it only runs while eqEnabled.
    float eqDelayMs = 0.0f;

    bool operator==(const Output&) const = default;
};

struct Route {
    std::uint32_t channelId = 0;
    std::uint32_t outputId = 0;
    float sendGainDb = 0.0f;

    bool operator==(const Route&) const = default;
};

struct Scene {
    int schemaVersion = 3;
    std::string name;
    double sampleRate = 48000;
    int bufferFrames = 256;
    std::vector<Channel> channels;
    std::vector<Output> outputs;
    std::vector<Route> routes;

    bool operator==(const Scene&) const = default;
};

} // namespace pluginrack
