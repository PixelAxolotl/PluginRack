#include <pluginrack/AudioDeviceProbe.h>

#include <algorithm>

namespace pluginrack {

int wasapiProbeCapValue(bool enabled, int channels) {
    if (!enabled) return 0;
    return std::clamp(channels, kWasapiProbeCapMinChannels,
                      kWasapiProbeCapMaxChannels);
}

} // namespace pluginrack
