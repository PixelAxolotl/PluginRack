#pragma once

#include <string_view>

namespace pluginrack {

// Assigned endpoint names that are not a device lookup. They travel through
// scenes, the settings page and the strips as plain strings, so the policy
// lives here once instead of being re-spelled at every compare.
inline constexpr std::string_view kSilentDevice = "None - silent";
inline constexpr std::string_view kSystemDefaultDevice = "System Default";

// No endpoint: the strip renders silence rather than an error.
inline bool isSilentDevice(std::string_view deviceName) {
    return deviceName.empty() || deviceName == kSilentDevice;
}

// Let the system pick the endpoint; the opener passes an empty name to JUCE.
inline bool isSystemDefaultDevice(std::string_view deviceName) {
    return deviceName == kSystemDefaultDevice;
}

} // namespace pluginrack
