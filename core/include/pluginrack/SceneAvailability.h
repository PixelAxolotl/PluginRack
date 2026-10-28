#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pluginrack/DeviceRegistry.h"
#include "pluginrack/Scene.h"

namespace pluginrack {

struct SceneAvailability {
    // Scene-order indices (0-based) into scene.channels; not channel ids.
    std::vector<std::uint32_t> stoppedChannels;
    // Scene-order indices (0-based) into scene.outputs; not output ids.
    std::vector<std::uint32_t> disabledOutputs;
    // One per missing device, "<kind> '<deviceId>' unavailable".
    std::vector<std::string> warnings;
};

SceneAvailability resolveAvailability(const Scene& scene, const DeviceRegistry& registry);

} // namespace pluginrack
