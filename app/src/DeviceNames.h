#pragma once

#include <JuceHeader.h>

#include <pluginrack/DeviceNames.h>

// JUCE-string face of the device-name policy in pluginrack/DeviceNames.h. The
// engine and the device menus compare endpoint names constantly; keeping the
// literals in one core header is what stops "None - silent" from being
// re-spelled at each compare.
inline juce::String deviceNameText(std::string_view name) {
    return juce::String(name.data(), static_cast<int>(name.size()));
}

inline juce::String silentDeviceName() {
    return deviceNameText(pluginrack::kSilentDevice);
}

inline juce::String systemDefaultDeviceName() {
    return deviceNameText(pluginrack::kSystemDefaultDevice);
}

inline bool isSilentDeviceName(const juce::String& name) {
    return pluginrack::isSilentDevice(name.toRawUTF8());
}

inline bool isSystemDefaultName(const juce::String& name) {
    return pluginrack::isSystemDefaultDevice(name.toRawUTF8());
}
