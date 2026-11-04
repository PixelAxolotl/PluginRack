#pragma once

#include <string_view>
#include <vector>

namespace pluginrack {

enum class StreamRole {
    Clock,
    Output,
    Input,
};

struct DeviceTypeNames {
    static constexpr std::string_view shared = "Windows Audio";
    static constexpr std::string_view sharedLowLatency = "Windows Audio (Low Latency Mode)";
    static constexpr std::string_view exclusive = "Windows Audio (Exclusive Mode)";
    static constexpr std::string_view asio = "ASIO";
};

// Returns the ordered list of type names to probe, best first.
// asioAvailable: the platform/build actually provides ASIO (JUCE_ASIO compiled in).
inline std::vector<std::string_view> deviceTypeOrder(StreamRole role,
                                                    bool preferExclusive,
                                                    bool asioAvailable) {
    std::vector<std::string_view> order;
    order.reserve(4);

    if (role != StreamRole::Clock) {
        // Captures and non-clock outputs must stay shareable, so exclusive mode is
        // never offered for them: it would lock every other application, including
        // the one the clock stream runs in, out of the endpoint.
        order.push_back(DeviceTypeNames::shared);
        order.push_back(DeviceTypeNames::sharedLowLatency);
        if (asioAvailable) {
            order.push_back(DeviceTypeNames::asio);
        }
        return order;
    }

    if (preferExclusive) {
        // The user asked for exclusive mode, so it is probed before anything else.
        order.push_back(DeviceTypeNames::exclusive);
        if (asioAvailable) {
            order.push_back(DeviceTypeNames::asio);
        }
        order.push_back(DeviceTypeNames::sharedLowLatency);
        order.push_back(DeviceTypeNames::shared);
        return order;
    }

    if (asioAvailable) {
        // ASIO drives the hardware directly, which gives the steadiest clock.
        order.push_back(DeviceTypeNames::asio);
    }
    order.push_back(DeviceTypeNames::sharedLowLatency);
    order.push_back(DeviceTypeNames::shared);
    // Exclusive mode is the clock's last resort: it locks other apps out, which is
    // only acceptable when shared types cannot be opened at all.
    order.push_back(DeviceTypeNames::exclusive);
    return order;
}

} // namespace pluginrack
