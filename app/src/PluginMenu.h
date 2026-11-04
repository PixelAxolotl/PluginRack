#pragma once

#include <JuceHeader.h>

#include <functional>

namespace PluginMenu {

// Popup listing the installed plugins as FORMAT > VENDOR > PLUGIN, followed by
// the folder actions and a file-browser fallback. `onChoose` receives the
// plugin bundle the user picked and the uniqueId of the class inside it
// (0 when the caller has to fall back to the bundle's first class).
void showAddPluginMenu(juce::Component& anchor,
                       std::function<void(const juce::File&, int uniqueId)> onChoose);

} // namespace PluginMenu
