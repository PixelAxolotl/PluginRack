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

// Puts the plugin's native window at the top of its sibling stack. Plugin
// views are separate HWNDs, so JUCE's own component z-order does not move
// them; the outgoing editor needs this to stay above an incoming one while
// the new editor attaches.
void raiseWindow(void* windowHandle);

// Puts the plugin's native window at the bottom of its sibling stack, so the
// outgoing editor stays visible on top while the incoming one is created and
// laid out. The new window keeps painting underneath and is raised by the
// reveal.
void lowerWindow(void* windowHandle);

// Places a window directly below another sibling, without showing or moving
// it. Used to put the incoming editor behind the outgoing one before it is
// shown, so the OS cannot composite an unpainted frame on top.
void placeWindowBelow(void* windowHandle, void* siblingHandle);

// Hides a plugin's native window (and the view inside it) without touching
// the JUCE component. The incoming editor is kept hidden while it is fitted:
// a VST3 view paints white at its unscaled size until it has handled the
// resize, and the outgoing editor stays on screen meanwhile.
void hideWindow(void* windowHandle);
void showWindowNoActivate(void* windowHandle);

// Shrinks |target|'s window region to the overlap with |cover| (both are
// top-level, sibling windows). Used so an incoming editor that has not been
// scaled yet cannot show its unscaled white/black overflow around the editor
// it is replacing.
void clipWindowToWindow(void* targetHandle, void* coverHandle);
void clearWindowClip(void* windowHandle);

// Hides visible JUCE child windows of the app that sit above its top edge.
// Some plugins create their editor window at its natural size and leave it
// parked off screen; because it is a child of the app window, the OS
// composites its unpainted white surface over the top bar. Returns the number
// of windows hidden (usually 0).
int hideOffscreenJuceChildren(void* mainWindowHandle, int* visibleJuceChildren = nullptr);

// Hides every visible JUCE window of the app whose rectangle intersects
// |screenRect|, except |keepWindow|. Some plugins expose their editor as more
// than one native window; hiding only the window returned by pluginWindowOf()
// leaves another copy on screen (the "two plugins at once" / white-bar bug).
// Pass keepWindow = nullptr to hide everything in the rectangle.
juce::Rectangle<int> windowRect(void* windowHandle);

// Captures a rectangle of the desktop (physical pixels). Used to freeze the
// outgoing editor as a still image while a new one is prepared, so the bay
// never shows an empty/white window during the swap.
juce::Image captureScreenRegion(int x, int y, int width, int height);

// Renders the whole window (including child windows) with PrintWindow. Unlike
// captureWindow() this is safe to call for a window owned by the caller and
// keeps near-uniform results, because it is used for a full-window freeze
// frame rather than a thumbnail.
juce::Image captureWindowForOverlay(void* windowHandle);

// Shrinks a window to a 1x1 region so it keeps painting while staying invisible.
void clipWindowToTiny(void* windowHandle);

int hideJuceWindowsInRect(void* mainWindowHandle, void* keepWindow,
                          void* keepWindow2, int left, int top, int right,
                          int bottom);

// Hide every visible top-level window of the main window's process except the
// main window and up to two kept handles. A plugin editor can create its own
// top-level window at the screen origin while it is being created; that window
// never intersects the bay, so the rect-based hide above cannot catch it.
int hideOtherProcessWindows(void* mainWindowHandle, void* keepWindow, void* keepWindow2);

// Flushes the window's pending paint immediately. A newly raised plugin
// window has not painted yet, and its first frame is black; painting it in
// the same message turn as the reveal keeps that frame off the screen.
void repaintWindowNow(void* windowHandle);

// A plain top-level window kept directly below the app window. Plugin windows
// are re-parented into it, so they keep painting normally for PrintWindow while
// the app window covers them. This is the only way to keep a VST3 window out of
// sight: it is a child window (JUCE parents it to the app), and a child always
// paints above its parent's content.
void* createHiddenHost();
void destroyHiddenHost(void* hostWindow);
// Places the host exactly under the app window and moves the plugin's window
// into it. Called every capture turn, because JUCE re-parents its own window
// whenever the component moves or resizes.
void parkBehindApp(void* pluginWindow, void* appWindow, void* hostWindow);

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
