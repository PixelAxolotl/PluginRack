#pragma once

#include <JuceHeader.h>

#include <functional>

namespace PluginMenu {

// Popup listing the installed plugins as FORMAT > VENDOR > PLUGIN, followed by
// the folder actions and a file-browser fallback. `onChoose` receives the
// plugin bundle the user picked.
void showAddPluginMenu(juce::Component& anchor, std::function<void(const juce::File&)> onChoose);

} // namespace PluginMenu
