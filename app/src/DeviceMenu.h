#pragma once

#include <JuceHeader.h>

#include <functional>

namespace DeviceMenu {
// The list is the same for both directions; only the header differs.
void showDeviceMenu(bool forInput, juce::Component& target, const juce::String& current,
                    const juce::StringArray& devices,
                    std::function<void(juce::String)> onChoose);
}
