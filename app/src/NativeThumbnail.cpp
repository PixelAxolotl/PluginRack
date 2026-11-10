#include "NativeThumbnail.h"

#include <cstdint>
#include <cstring>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace nativedisplay {

void* pluginWindowOf(juce::Component& editor) {
   #if JUCE_WINDOWS
    if (auto* embedded = dynamic_cast<juce::HWNDComponent*>(&editor))
        return embedded->getHWND();
    for (auto* child : editor.getChildren())
        if (auto* found = pluginWindowOf(*child))
            return found;
   #else
    juce::ignoreUnused(editor);
   #endif
    return nullptr;
}

#if JUCE_WINDOWS
namespace {

// PrintWindow's PW_RENDERFULLCONTENT: without it a window that composes
// through DirectComposition (most plugin UIs) returns an empty bitmap.
constexpr UINT kRenderFullContent = 0x00000002;

// Copies a top-down 32-bit DIB into a JUCE image, forcing opaque alpha (DIB
// alpha is usually zero, which would make the result invisible) and reporting
// whether every pixel came back identical - the signature of a failed capture.
juce::Image toImage(const void* dibPixels, int width, int height, bool& uniform) {
    juce::Image image(juce::Image::ARGB, width, height, false);
    juce::Image::BitmapData dest(image, juce::Image::BitmapData::writeOnly);
    const auto* source = static_cast<const std::uint8_t*>(dibPixels);

    std::uint32_t firstPixel = 0;
    bool first = true;
    uniform = true;

    for (int y = 0; y < height; ++y) {
        auto* row = dest.getLinePointer(y);
        const auto* srcRow = source + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            // DIB is BGRA byte order, which is exactly how PixelARGB is laid
            // out on little-endian Windows; only the alpha byte needs fixing.
            const std::uint32_t pixel = static_cast<std::uint32_t>(srcRow[x * 4])
                                        | (static_cast<std::uint32_t>(srcRow[x * 4 + 1]) << 8)
                                        | (static_cast<std::uint32_t>(srcRow[x * 4 + 2]) << 16);
            if (first) {
                firstPixel = pixel;
                first = false;
            } else if (pixel != firstPixel) {
                uniform = false;
            }
            std::memcpy(row + static_cast<std::size_t>(x) * 4, srcRow + x * 4, 3);
            row[static_cast<std::size_t>(x) * 4 + 3] = 0xff;
        }
    }
    return image;
}

} // namespace
#endif

juce::Image captureWindow(void* windowHandle, int targetWidth) {
   #if JUCE_WINDOWS
    auto* window = static_cast<HWND>(windowHandle);
    if (window == nullptr || IsWindow(window) == FALSE) return {};

    // Never capture a window on the thread that owns it. PrintWindow asks
    // the owning thread to repaint, so calling it from the message thread
    // that owns the window's host component deadlocks a host that waits for
    // the repaint that only its own thread can perform.
    if (GetWindowThreadProcessId(window, nullptr) == ::GetCurrentThreadId())
        return {};

    // A hidden window can never repaint anything meaningful.
    if (::IsWindowVisible(window) == FALSE) return {};

    RECT client{};
    if (GetClientRect(window, &client) == FALSE) return {};
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return {};

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) return {};
    HDC memoryDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = memoryDc != nullptr ? CreateCompatibleBitmap(screenDc, width, height) : nullptr;
    if (memoryDc == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memoryDc != nullptr) DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        return {};
    }

    const HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    const BOOL rendered = PrintWindow(window, memoryDc, kRenderFullContent);

    juce::Image image;
    if (rendered != FALSE) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // negative: rows top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width)
                                         * static_cast<std::size_t>(height) * 4);
        if (GetDIBits(memoryDc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info,
                      DIB_RGB_COLORS)
            != 0) {
            bool uniform = true;
            const auto captured = toImage(pixels.data(), width, height, uniform);
            if (!uniform) image = captured;
        }
    }

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);

    if (image.isNull()) return {};
    if (targetWidth > 0 && targetWidth < image.getWidth())
        image = image.rescaled(targetWidth, juce::jmax(1, image.getHeight() * targetWidth / image.getWidth()));
    return image;
   #else
    juce::ignoreUnused(windowHandle, targetWidth);
    return {};
   #endif
}

void* createHiddenHost() {
   #if JUCE_WINDOWS
    const auto instance = ::GetModuleHandleW(nullptr);
    constexpr const wchar_t* className = L"PluginRackPreviewHost";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    ::RegisterClassExW(&wc); // harmless when it already exists
    return ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, className, L"",
                             WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, nullptr, nullptr,
                             instance, nullptr);
   #else
    return nullptr;
   #endif
}

void destroyHiddenHost(void* hostWindow) {
   #if JUCE_WINDOWS
    if (auto* window = static_cast<HWND>(hostWindow))
        ::DestroyWindow(window);
   #else
    juce::ignoreUnused(hostWindow);
   #endif
}

void parkBehindApp(void* pluginWindow, void* appWindow, void* hostWindow) {
   #if JUCE_WINDOWS
    auto* plugin = static_cast<HWND>(pluginWindow);
    auto* app = static_cast<HWND>(appWindow);
    auto* host = static_cast<HWND>(hostWindow);
    if (plugin == nullptr || app == nullptr || host == nullptr) return;
    if (::IsWindow(plugin) == FALSE || ::IsWindow(app) == FALSE) return;

    // While the app is minimised or hidden there is nothing to hide behind:
    // a visible host would sit on the desktop as a black rectangle.
    if (::IsWindowVisible(app) == FALSE || ::IsIconic(app) != FALSE) {
        ::ShowWindow(host, SW_HIDE);
        return;
    }

    RECT appRect{};
    if (::GetWindowRect(app, &appRect) == FALSE) return;

    if (::GetParent(plugin) != host)
        ::SetParent(plugin, host);

    // Show first, place second. ShowWindow raises the window, so positioning it
    // below the app before showing left it on top of the UI.
    ::ShowWindow(host, SW_SHOWNOACTIVATE);
    ::SetWindowPos(host, app, appRect.left, appRect.top,
                   appRect.right - appRect.left, appRect.bottom - appRect.top,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    ::SetWindowPos(plugin, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
   #else
    juce::ignoreUnused(pluginWindow, appWindow, hostWindow);
   #endif
}

namespace {

// The plugin paints on its own thread while PrintWindow waits, so the gap
// after a capture is measured in what that capture actually cost: three times
// the cost keeps the repaint work to a quarter of the time.
constexpr int kCaptureCostFactor = 3;

} // namespace

WindowCaptureWorker::WindowCaptureWorker()
    : juce::Thread("PluginRack window capture") {
    startThread();
}

WindowCaptureWorker::~WindowCaptureWorker() {
    stop();
}

void WindowCaptureWorker::request(void* windowHandle, int targetWidth,
                                  std::function<void(juce::Image)> onCaptured) {
    if (windowHandle == nullptr) {
        clearRequest();
        return;
    }
    {
        const juce::ScopedLock lock(requestLock);
        pendingHandle = windowHandle;
        pendingWidth = targetWidth;
        pendingCallback = std::move(onCaptured);
    }
    wake.signal();
}

void WindowCaptureWorker::clearRequest() {
    const juce::ScopedLock lock(requestLock);
    pendingHandle = nullptr;
    pendingCallback = nullptr;
}

void WindowCaptureWorker::stop() {
    clearRequest();
    if (!isThreadRunning()) return;
    signalThreadShouldExit();
    wake.signal();
    stopThread(2000);
}

void WindowCaptureWorker::run() {
    while (!threadShouldExit()) {
        void* handle = nullptr;
        int width = 0;
        std::function<void(juce::Image)> callback;
        {
            const juce::ScopedLock lock(requestLock);
            handle = pendingHandle;
            width = pendingWidth;
            callback = std::move(pendingCallback);
            pendingHandle = nullptr;
            pendingCallback = nullptr;
            // Published under the same lock the message thread uses to clear a
            // request, so isCapturing() can never read "idle" while a capture
            // is just starting.
            capturing.set(handle != nullptr ? 1 : 0);
        }

        if (handle == nullptr) {
            wake.wait(100);
            continue;
        }

        const auto started = juce::Time::getMillisecondCounterHiRes();
        // No lock is held across this call: PrintWindow asks the window's
        // owning thread to paint, and that thread may be the one that will
        // later destroy this window.
        juce::Image image = captureWindow(handle, width);
        capturing.set(0);
        const auto costMs =
            juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - started);

        if (callback != nullptr) {
            juce::MessageManager::callAsync(
                [done = std::move(callback), captured = image]() mutable {
                    done(captured);
                });
        }

        if (threadShouldExit()) break;
        juce::Thread::sleep(juce::jmax(1, costMs * kCaptureCostFactor));
    }
}

} // namespace nativedisplay
