#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pluginrack/Types.h"

namespace pluginrack {

struct InsertSlot {
    std::uint32_t id = 0;
    std::string pluginPath;
    std::string pluginName;
    bool bypassed = false;

    bool operator==(const InsertSlot&) const = default;
};

struct Channel {
    std::uint32_t id = 0;
    std::string name;
    SourceKind sourceKind = SourceKind::Hardware;
    std::string sourceDeviceId;
    ChannelLayout layout = ChannelLayout::Mono;
    float gainDb = 0.0f;
    float pan = 0.0f;
    bool muted = false;
    bool soloed = false;
    std::vector<InsertSlot> inserts;

    bool operator==(const Channel&) const = default;
};

struct Output {
    std::uint32_t id = 0;
    std::string name;
    OutputKind kind = OutputKind::Physical;
    std::string deviceId;
    std::vector<InsertSlot> inserts;

    bool operator==(const Output&) const = default;
};

struct Route {
    std::uint32_t channelId = 0;
    std::uint32_t outputId = 0;
    float sendGainDb = 0.0f;

    bool operator==(const Route&) const = default;
};

struct Scene {
    int schemaVersion = 2;
    std::string name;
    double sampleRate = 48000;
    int bufferFrames = 256;
    std::vector<Channel> channels;
    std::vector<Output> outputs;
    std::vector<Route> routes;

    bool operator==(const Scene&) const = default;
};

} // namespace pluginrack
