#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <pluginrack/AsioDeviceMatch.h>

namespace pluginrack {

// Input strips that belong to the same interface the clock just moved to.
//
// Moving the clock to ASIO is not enough to remove the capture latency: the
// strip still points at the Windows endpoint name, so the duplex match
// ("this capture is on the clock device") never fires and the input keeps its
// own client and its own read delay. On a single-client driver the second
// client is refused outright.
//
// `inputs` is (strip id, assigned device name). A strip is retargeted when its
// endpoint resolves to the same ASIO driver as the clock, and it is left alone
// when it is already on that driver or when it resolves elsewhere.
inline std::vector<std::uint32_t> inputsToRetargetForClock(
    const std::vector<std::pair<std::uint32_t, std::string>>& inputs,
    const std::string& clockDevice, const std::vector<std::string>& asioDevices) {
    std::vector<std::uint32_t> moved;
    if (clockDevice.empty()) return moved;

    for (const auto& [id, deviceName] : inputs) {
        if (deviceName.empty() || deviceName == "None - silent") continue;
        if (deviceName == clockDevice) continue;
        if (bestAsioDeviceMatch(deviceName, asioDevices) == clockDevice) moved.push_back(id);
    }
    return moved;
}

// When the clock's own endpoint has no ASIO driver, the only way to remove the
// capture read delay is to put the clock on the interface the inputs already
// use. Returns that driver when the inputs agree on exactly one; empty when
// there are none or several (a guess would break someone's routing).
inline std::string asioClockFromInputs(const std::vector<std::string>& inputDevices,
                                       const std::vector<std::string>& asioDevices) {
    std::string found;
    for (const auto& device : inputDevices) {
        if (device.empty() || device == "None - silent") continue;
        const auto match = bestAsioDeviceMatch(device, asioDevices);
        if (match.empty()) continue;
        if (match == device) {
            // The input is already on an ASIO driver.
            if (!found.empty() && found != device) return {};
            found = device;
            continue;
        }
        if (!found.empty() && found != match) return {};
        found = match;
    }
    return found;
}

} // namespace pluginrack
