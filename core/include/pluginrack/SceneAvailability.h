#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pluginrack/DeviceRegistry.h"
#include "pluginrack/Scene.h"

namespace pluginrack {

struct SceneAvailability {
    std::vector<std::uint32_t> stoppedChannels;
    std::vector<std::uint32_t> disabledOutputs;
    std::vector<std::string> warnings;
};

SceneAvailability resolveAvailability(const Scene& scene, const DeviceRegistry& registry);

} // namespace pluginrack
