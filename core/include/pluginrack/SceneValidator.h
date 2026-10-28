#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pluginrack/Scene.h"

namespace pluginrack {

enum class SceneErrorCode {
    DuplicateChannelId,
    DuplicateOutputId,
    DuplicateRoute,
    UnknownChannelInRoute,
    UnknownOutputInRoute,
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
