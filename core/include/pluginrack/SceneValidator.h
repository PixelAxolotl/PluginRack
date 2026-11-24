#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pluginrack/Scene.h"

namespace pluginrack {

enum class SceneErrorCode {
    DuplicateChannelId,
    DuplicateOutputId,
    DuplicateBusId,
    DuplicateRoute,
    DuplicateBusSend,
    DuplicateBusRoute,
    DuplicateInsertId,
    DuplicateEffectId,
    DuplicateChainItem,
    UnknownChannelInRoute,
    UnknownOutputInRoute,
    UnknownChannelInSend,
    UnknownBusInSend,
    UnknownBusInRoute,
    UnknownOutputInBusRoute,
    UnknownChainItem,
    UnknownTapItem,
    UnknownWatchedDevice,
    UnknownSideChainSourceId,
    UnsupportedSchemaVersion,
    ValueOutOfRange,
    NoChannels,
    NoOutputs
};

struct SceneError {
    SceneErrorCode code;
    std::uint32_t entityId;
    std::string message;
};

std::vector<SceneError> validate(const Scene& scene);

} // namespace pluginrack
