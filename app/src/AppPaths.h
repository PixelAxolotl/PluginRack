#pragma once

#include <JuceHeader.h>

// Every file the app persists lives under one root. Spelling the product folder
// out at each call site meant a rename or a move split the app's state across
// two directories with no error to show for it.
namespace appPaths {

inline juce::File dataRoot() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("PluginRack");
}

// A path under the data root, e.g. file("plugins/faults.json").
inline juce::File file(const juce::String& relativePath) {
    return dataRoot().getChildFile(relativePath);
}

} // namespace appPaths
