#pragma once

#include <JuceHeader.h>

#include <functional>

#include "EQCurveComponent.h"

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
