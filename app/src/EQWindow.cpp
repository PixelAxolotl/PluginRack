#include "EQWindow.h"

#include "Theme.h"

EQWindow::EQWindow(const juce::String& title)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons) {
    setUsingNativeTitleBar(true);
    setContentOwned(&curve, false);
    setResizable(true, true);
    setResizeLimits(360, 240, 1000, 700);
    centreWithSize(kWidth, kHeight);
    setVisible(true);
}

void EQWindow::closeButtonPressed() {
    // Deleting a window from inside its own close-button callback is fragile:
    // defer so the button can finish dispatching first. The owner may destroy
    // the window before the message arrives, so guard with a weak pointer.
    juce::Component::SafePointer<EQWindow> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        if (safe->onCloseRequest) safe->onCloseRequest();
    });
}
