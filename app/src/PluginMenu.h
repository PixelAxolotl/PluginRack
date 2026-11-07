#pragma once

#include <JuceHeader.h>

#include <functional>

#include "pluginrack/DynamicsEffect.h"
#include "pluginrack/Scene.h"

namespace PluginMenu {

// Built-in effects a strip can move into the picked slot. The descriptor
// carries the chain item the host moves and its display name; the host (not
// the menu) knows the strip's current chain.
using BuiltInCallback = std::function<void(pluginrack::ChainItem item)>;

// Popup listing the installed plugins as FORMAT > VENDOR > PLUGIN, followed by
// the folder actions and a file-browser fallback. `onChoose` receives the
// plugin bundle the user picked and the uniqueId of the class inside it
// (0 when the caller has to fall back to the bundle's first class).
void showAddPluginMenu(juce::Component& anchor,
                       std::function<void(const juce::File&, int uniqueId)> onChoose);

// Same menu with a Built-In effects submenu. Built-in effects are singletons
// per strip, so picking one moves that effect to the slot the menu was opened
// from rather than adding a second copy.
void showAddPluginMenu(juce::Component& anchor,
                       std::function<void(const juce::File&, int uniqueId)> onChoose,
                       BuiltInCallback onBuiltIn);

} // namespace PluginMenu
