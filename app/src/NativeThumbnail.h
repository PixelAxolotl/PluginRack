#pragma once

#include <JuceHeader.h>

#include <functional>

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

// Pushes a native window to the bottom of the z-order, so the app window
// covers it while it is captured. A plugin window that is off-screen renders
// nothing to PrintWindow, so background previews park their windows under the
// app instead of far away from it.
void sendToBack(void* windowHandle);

// Captures a native window on a worker thread. PrintWindow makes the target
// window paint its whole UI, and that paint is far too slow to sit inside a
// message-thread timer at display rates: it starved every other message and
// the window stopped responding while audio kept running.
//
// Only one capture is ever in flight and only the newest pending request
// survives, so a slow window lowers the capture rate instead of queueing
// work. After each capture the worker waits three times the measured cost,
// which keeps the plugin's repaint work to a quarter of the time even when
// the caller asks for a faster rate.
class WindowCaptureWorker final : private juce::Thread {
public:
    WindowCaptureWorker();
    ~WindowCaptureWorker() override;

    // Replaces any pending request and returns immediately.
    void request(void* windowHandle, int targetWidth,
                 std::function<void(juce::Image)> onCaptured);
    void clearRequest();
    void stop();

    // True while the worker is inside PrintWindow. Never block the message
    // thread waiting for this: PrintWindow asks the window's owning thread
    // (the message thread) to paint, so a blocking wait deadlocks.
    bool isCapturing() const { return capturing.get() != 0; }

private:
    void run() override;

    juce::CriticalSection requestLock;
    void* pendingHandle = nullptr;
    int pendingWidth = 0;
    std::function<void(juce::Image)> pendingCallback;
    // Published under requestLock, cleared when the capture finishes.
    juce::Atomic<int> capturing{ 0 };
    juce::WaitableEvent wake{ false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WindowCaptureWorker)
};

} // namespace nativedisplay
