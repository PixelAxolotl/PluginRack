#include "pluginrack/SceneAvailability.h"

namespace pluginrack {

SceneAvailability resolveAvailability(const Scene& scene, const DeviceRegistry& registry) {
    SceneAvailability availability;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(scene.channels.size()); ++i) {
        if (!registry.hasInput(scene.channels[i].sourceDeviceId)) {
            availability.stoppedChannels.push_back(i);
            availability.warnings.push_back("input '" + scene.channels[i].sourceDeviceId +
                                            "' unavailable");
        }
    }
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(scene.outputs.size()); ++i) {
        if (!registry.hasOutput(scene.outputs[i].deviceId)) {
            availability.disabledOutputs.push_back(i);
            availability.warnings.push_back("output '" + scene.outputs[i].deviceId +
                                            "' unavailable");
        }
    }
    return availability;
}

} // namespace pluginrack
