#pragma once

#include <JuceHeader.h>

// A JUCE-hosted VST3 editor is not drawn by JUCE: the plugin lives in its own
// native window and the hosting component's paint() is a black fill (see
// juce_VST3PluginFormat.cpp, VST3PluginWindow::ViewComponent), so
// createComponentSnapshot can never see a plugin's pixels. These helpers find
// that window and copy the real contents instead.
namespace nativedisplay {

// Native window inside a hosted editor, or nullptr when the editor draws
// itself in software (a component snapshot then works).
void* pluginWindowOf(juce::Component& editor);

// Copies the window's pixels, scaled down to targetWidth. Empty when the
// window cannot be captured or came back as one flat colour.
juce::Image captureWindow(void* windowHandle, int targetWidth);

} // namespace nativedisplay
