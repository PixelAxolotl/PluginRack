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

// Short label for the settings page, the strips and the latency budget; these
// answer "am I on ASIO or WASAPI right now?" without the long registered name.
enum class DeviceBackend {
    Shared,
    SharedLowLatency,
    Exclusive,
    Asio,
    Unknown,
};

inline DeviceBackend deviceBackendOf(std::string_view typeName) {
    if (typeName == DeviceTypeNames::asio) return DeviceBackend::Asio;
    if (typeName == DeviceTypeNames::exclusive) return DeviceBackend::Exclusive;
    if (typeName == DeviceTypeNames::sharedLowLatency) return DeviceBackend::SharedLowLatency;
    if (typeName == DeviceTypeNames::shared) return DeviceBackend::Shared;
    return DeviceBackend::Unknown;
}

// Empty for an unknown type, so the caller can show the name it was given.
inline std::string_view deviceBackendTag(DeviceBackend backend) {
    switch (backend) {
        case DeviceBackend::Asio: return "ASIO";
        case DeviceBackend::SharedLowLatency: return "WASAPI LL";
        case DeviceBackend::Exclusive: return "WASAPI excl";
        case DeviceBackend::Shared: return "WASAPI";
        case DeviceBackend::Unknown: break;
    }
    return {};
}

// The mode id persisted with a device: the opener compares it against the mode
// it selected, so these strings are a file format.
inline std::string_view deviceModeId(DeviceBackend backend) {
    switch (backend) {
        case DeviceBackend::Asio: return "asio";
        case DeviceBackend::SharedLowLatency: return "sharedLowLatency";
        case DeviceBackend::Exclusive: return "exclusive";
        case DeviceBackend::Shared: return "shared";
        case DeviceBackend::Unknown: break;
    }
    return {};
}

} // namespace pluginrack
