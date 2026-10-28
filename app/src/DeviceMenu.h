#pragma once

#include <JuceHeader.h>

#include <functional>

namespace DeviceMenu {
void showInputMenu(juce::Component& target, const juce::String& current,
                   const juce::StringArray& devices,
                   std::function<void(juce::String)> onChoose);
void showOutputMenu(juce::Component& target, const juce::String& current,
                    const juce::StringArray& devices,
                    std::function<void(juce::String)> onChoose);
}
