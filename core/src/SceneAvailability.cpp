#include "pluginrack/SceneAvailability.h"

#include <unordered_set>

namespace pluginrack {

SceneAvailability resolveAvailability(const Scene& scene, const DeviceRegistry& registry) {
    SceneAvailability availability;
    std::unordered_set<std::string> seenInputs;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(scene.channels.size()); ++i) {
        if (!registry.hasInput(scene.channels[i].sourceDeviceId)) {
            availability.stoppedChannels.push_back(i);
            if (seenInputs.insert(scene.channels[i].sourceDeviceId).second) {
                availability.warnings.push_back("input '" + scene.channels[i].sourceDeviceId +
                                                "' unavailable");
            }
        }
    }
    std::unordered_set<std::string> seenOutputs;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(scene.outputs.size()); ++i) {
        if (!registry.hasOutput(scene.outputs[i].deviceId)) {
            availability.disabledOutputs.push_back(i);
            if (seenOutputs.insert(scene.outputs[i].deviceId).second) {
                availability.warnings.push_back("output '" + scene.outputs[i].deviceId +
                                                "' unavailable");
            }
        }
    }
    return availability;
}

} // namespace pluginrack
