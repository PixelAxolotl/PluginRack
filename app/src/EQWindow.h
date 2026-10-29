#pragma once

#include <JuceHeader.h>

#include <functional>

#include "EQCurveComponent.h"
#include "Theme.h"

// Task-4 shell button: stock TextButton face with a flag-driven DESIGN.md 1px
// accent-cyan ring (MiniButton/DeviceControl convention). The ring paints on
// top of the face so an amber-active face keeps precedence while open.
class EqButton final : public juce::TextButton {
public:
    explicit EqButton(const juce::String& text) : juce::TextButton(text) {}

    void setRingVisible(bool visible) {
        if (ringVisible == visible) return;
        ringVisible = visible;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        juce::TextButton::paint(g);
        if (ringVisible) {
            g.setColour(theme::cyan());
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.5f), 3.0f, 1.0f);
        }
    }

private:
    bool ringVisible = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqButton)
};

// Floating EQ editor shell (Task 4). Single instance per strip: the owner
// strip creates it in openEqWindow() and destroys it in closeEqWindow().
// Close-button handling mirrors PluginHost's PluginWindow: the delete is
// deferred through SafePointer + callAsync so the button can finish
// dispatching first, and the owner may be gone by the time it arrives.
class EQWindow final : public juce::DocumentWindow {
public:
    static constexpr int kWidth = 520;
    static constexpr int kHeight = 360;

    explicit EQWindow(const juce::String& title);

    void closeButtonPressed() override;

    std::function<void()> onCloseRequest;

private:
    EQCurveComponent curve;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EQWindow)
};
