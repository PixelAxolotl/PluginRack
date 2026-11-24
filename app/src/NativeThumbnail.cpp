#include "NativeThumbnail.h"

#include <cstdint>
#include <cstring>
#include <cwchar>
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

void raiseWindow(void* windowHandle) {
   #if JUCE_WINDOWS
    if (windowHandle == nullptr) return;
    SetWindowPos(static_cast<HWND>(windowHandle), HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
   #else
    juce::ignoreUnused(windowHandle);
   #endif
}

void lowerWindow(void* windowHandle) {
   #if JUCE_WINDOWS
    if (windowHandle == nullptr) return;
    SetWindowPos(static_cast<HWND>(windowHandle), HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
   #else
    juce::ignoreUnused(windowHandle);
   #endif
}

void placeWindowBelow(void* windowHandle, void* siblingHandle) {
   #if JUCE_WINDOWS
    if (windowHandle == nullptr || siblingHandle == nullptr) return;
    SetWindowPos(static_cast<HWND>(windowHandle), static_cast<HWND>(siblingHandle),
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
   #else
    juce::ignoreUnused(windowHandle, siblingHandle);
   #endif
}

void repaintWindowNow(void* windowHandle) {
   #if JUCE_WINDOWS
    if (windowHandle == nullptr) return;
    ::RedrawWindow(static_cast<HWND>(windowHandle), nullptr, nullptr,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
   #else
    juce::ignoreUnused(windowHandle);
   #endif
}

void hideWindow(void* windowHandle) {
   #if JUCE_WINDOWS
    if (windowHandle == nullptr) return;
    ShowWindow(static_cast<HWND>(windowHandle), SW_HIDE);
   #else
    juce::ignoreUnused(windowHandle);
   #endif
}

void showWindowNoActivate(void* windowHandle) {
   #if JUCE_WINDOWS
    if (windowHandle == nullptr) return;
    ShowWindow(static_cast<HWND>(windowHandle), SW_SHOWNA);
   #else
    juce::ignoreUnused(windowHandle);
   #endif
}


void clipWindowToWindow(void* targetHandle, void* coverHandle) {
   #if JUCE_WINDOWS
    if (targetHandle == nullptr || coverHandle == nullptr) return;
    auto* target = static_cast<HWND>(targetHandle);
    auto* cover = static_cast<HWND>(coverHandle);
    RECT t {}, c {};
    if (! GetWindowRect(target, &t) || ! GetWindowRect(cover, &c)) return;
    RECT overlap {};
    overlap.left = t.left > c.left ? t.left : c.left;
    overlap.top = t.top > c.top ? t.top : c.top;
    overlap.right = t.right < c.right ? t.right : c.right;
    overlap.bottom = t.bottom < c.bottom ? t.bottom : c.bottom;
    if (overlap.right <= overlap.left || overlap.bottom <= overlap.top) return;
    POINT corners[2] { { overlap.left, overlap.top }, { overlap.right, overlap.bottom } };
    MapWindowPoints(HWND_DESKTOP, target, corners, 2);
    auto* region = CreateRectRgn(corners[0].x, corners[0].y,
                                 corners[1].x, corners[1].y);
    if (region != nullptr)
        SetWindowRgn(target, region, TRUE); // window takes ownership
   #else
    juce::ignoreUnused(targetHandle, coverHandle);
   #endif
}

void clearWindowClip(void* windowHandle) {
   #if JUCE_WINDOWS
    if (windowHandle == nullptr) return;
    SetWindowRgn(static_cast<HWND>(windowHandle), nullptr, TRUE);
   #else
    juce::ignoreUnused(windowHandle);
   #endif
}

#if JUCE_WINDOWS
namespace {
struct HideContext { RECT app; int hidden = 0; int seen = 0; };

void hideIfOffscreen(HWND window, HideContext& ctx, bool countSeen) {
    if (window == nullptr || IsWindowVisible(window) == FALSE) return;
    wchar_t className[64] {};
    if (GetClassNameW(window, className, 64) <= 0) return;
    if (std::wcsncmp(className, L"JUCE_", 5) != 0) return;
    RECT bounds {};
    if (GetWindowRect(window, &bounds) == FALSE) return;
    if (countSeen) ++ctx.seen;
    // Only windows parked above the app: a legitimate editor window lives
    // inside the app's client area, so this cannot hide the visible editor.
    if (bounds.bottom > bounds.top && bounds.top < ctx.app.top) {
        ShowWindow(window, SW_HIDE);
        ++ctx.hidden;
    }
}

BOOL CALLBACK hideChildCallback(HWND child, LPARAM param) {
    hideIfOffscreen(child, *reinterpret_cast<HideContext*>(param), true);
    return TRUE;
}

struct TopContext { HideContext* ctx; HWND main; };

BOOL CALLBACK hideTopCallback(HWND window, LPARAM param) {
    auto* top = reinterpret_cast<TopContext*>(param);
    if (window == top->main) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    hideIfOffscreen(window, *top->ctx, false);
    EnumChildWindows(window, hideChildCallback, reinterpret_cast<LPARAM>(top->ctx));
    return TRUE;
}
} // namespace
#endif

juce::Image captureScreenRegion(int x, int y, int width, int height) {
   #if JUCE_WINDOWS
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
    const HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    const BOOL copied = BitBlt(memoryDc, 0, 0, width, height, screenDc, x, y, SRCCOPY);
    juce::Image image;
    if (copied != FALSE) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width)
                                         * static_cast<std::size_t>(height) * 4);
        if (GetDIBits(memoryDc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info,
                      DIB_RGB_COLORS) != 0) {
            bool uniform = false;
            image = toImage(pixels.data(), width, height, uniform);
        }
    }
    SelectObject(memoryDc, previous);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    return image;
   #else
    juce::ignoreUnused(x, y, width, height);
    return {};
   #endif
}

juce::Image captureWindowForOverlay(void* windowHandle) {
   #if JUCE_WINDOWS
    auto* window = static_cast<HWND>(windowHandle);
    if (window == nullptr || IsWindow(window) == FALSE) return {};
    RECT rect{};
    if (GetWindowRect(window, &rect) == FALSE) return {};
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
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
    const HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    juce::Image image;
    if (PrintWindow(window, memoryDc, kRenderFullContent) != FALSE) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width)
                                         * static_cast<std::size_t>(height) * 4);
        if (GetDIBits(memoryDc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info,
                      DIB_RGB_COLORS) != 0) {
            bool uniform = false;
            image = toImage(pixels.data(), width, height, uniform);
        }
    }
    SelectObject(memoryDc, previous);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    return image;
   #else
    juce::ignoreUnused(windowHandle);
    return {};
   #endif
}

void clipWindowToTiny(void* windowHandle) {
   #if JUCE_WINDOWS
    if (windowHandle == nullptr) return;
    auto* region = CreateRectRgn(0, 0, 1, 1);
    if (region != nullptr)
        SetWindowRgn(static_cast<HWND>(windowHandle), region, TRUE);
   #else
    juce::ignoreUnused(windowHandle);
   #endif
}

juce::Rectangle<int> windowRect(void* windowHandle) {
   #if JUCE_WINDOWS
    RECT r {};
    if (windowHandle != nullptr && GetWindowRect(static_cast<HWND>(windowHandle), &r) != FALSE)
        return { r.left, r.top, r.right - r.left, r.bottom - r.top };
   #else
    juce::ignoreUnused(windowHandle);
   #endif
    return {};
}

#if JUCE_WINDOWS
namespace {
struct RectHideContext {
    RECT area {};
    HWND main = nullptr;
    HWND keep = nullptr;
    HWND keep2 = nullptr;
    DWORD processId = 0;
    int hidden = 0;
};

void hideIfInRect(HWND window, RectHideContext& ctx) {
    if (window == nullptr || window == ctx.main) return;
    if (window == ctx.keep || window == ctx.keep2) return;
    // Never hide an ancestor of a window we are keeping: pluginWindowOf() can
    // return the plugin's view, whose parent is the JUCE host window that is
    // actually on screen.
    if (ctx.keep != nullptr && IsChild(window, ctx.keep)) return;
    if (ctx.keep2 != nullptr && IsChild(window, ctx.keep2)) return;
    // ...and never hide a child of one either: the plugin's actual view is a
    // JUCE window nested inside the JUCE host window that we keep.
    if (ctx.keep != nullptr && IsChild(ctx.keep, window)) return;
    if (ctx.keep2 != nullptr && IsChild(ctx.keep2, window)) return;
    if (IsWindowVisible(window) == FALSE) return;
    // Any class, as long as it belongs to this process: Ozone hosts its editor
    // in iZ_* windows, not JUCE_* ones, and those are exactly the stray copies
    // that stayed on screen.
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != ctx.processId) return;
    RECT bounds {};
    if (GetWindowRect(window, &bounds) == FALSE) return;
    RECT overlap {};
    if (IntersectRect(&overlap, &bounds, &ctx.area) == FALSE) return;
    ShowWindow(window, SW_HIDE);
    ++ctx.hidden;
}

BOOL CALLBACK hideInRectCallback(HWND child, LPARAM param) {
    hideIfInRect(child, *reinterpret_cast<RectHideContext*>(param));
    return TRUE;
}

BOOL CALLBACK hideTopInRectCallback(HWND window, LPARAM param) {
    hideIfInRect(window, *reinterpret_cast<RectHideContext*>(param));
    return TRUE;
}

struct HideTopContext {
    HWND main = nullptr;
    HWND keep = nullptr;
    HWND keep2 = nullptr;
    DWORD processId = 0;
    int hidden = 0;
};

BOOL CALLBACK hideOtherTopCallback(HWND window, LPARAM param) {
    auto& ctx = *reinterpret_cast<HideTopContext*>(param);
    if (window == nullptr || window == ctx.main || window == ctx.keep || window == ctx.keep2)
        return TRUE;
    if (IsWindowVisible(window) == FALSE) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != ctx.processId) return TRUE;
    ShowWindow(window, SW_HIDE);
    ++ctx.hidden;
    return TRUE;
}
} // namespace
#endif

int hideJuceWindowsInRect(void* mainWindowHandle, void* keepWindow,
                          void* keepWindow2, int left, int top, int right,
                          int bottom) {
   #if JUCE_WINDOWS
    auto* main = static_cast<HWND>(mainWindowHandle);
    if (main == nullptr || right <= left || bottom <= top) return 0;
    RectHideContext ctx {};
    ctx.area = { left, top, right, bottom };
    ctx.main = main;
    ctx.keep = static_cast<HWND>(keepWindow);
    ctx.keep2 = static_cast<HWND>(keepWindow2);
    GetWindowThreadProcessId(main, &ctx.processId);
    EnumChildWindows(main, hideInRectCallback, reinterpret_cast<LPARAM>(&ctx));
    // Plugin editors can also be top-level owned popups rather than children
    // of the host window (Ozone does this); EnumChildWindows never sees those.
    EnumWindows(hideTopInRectCallback, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hidden;
   #else
    juce::ignoreUnused(mainWindowHandle, keepWindow, keepWindow2, left, top, right, bottom);
    return 0;
   #endif
}

int hideOtherProcessWindows(void* mainWindowHandle, void* keepWindow, void* keepWindow2) {
   #if JUCE_WINDOWS
    auto* main = static_cast<HWND>(mainWindowHandle);
    if (main == nullptr) return 0;
    HideTopContext ctx {};
    ctx.main = main;
    ctx.keep = static_cast<HWND>(keepWindow);
    ctx.keep2 = static_cast<HWND>(keepWindow2);
    GetWindowThreadProcessId(main, &ctx.processId);
    EnumWindows(hideOtherTopCallback, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hidden;
   #else
    juce::ignoreUnused(mainWindowHandle, keepWindow, keepWindow2);
    return 0;
   #endif
}

int hideOffscreenJuceChildren(void* mainWindowHandle, int* visibleJuceChildren) {
   #if JUCE_WINDOWS
    if (visibleJuceChildren != nullptr) *visibleJuceChildren = 0;
    auto* main = static_cast<HWND>(mainWindowHandle);
    if (main == nullptr) return 0;
    RECT app {};
    if (GetWindowRect(main, &app) == FALSE) return 0;

    HideContext context { app };
    EnumChildWindows(main, hideChildCallback, reinterpret_cast<LPARAM>(&context));
    TopContext top { &context, main };
    EnumWindows(hideTopCallback, reinterpret_cast<LPARAM>(&top));
    if (visibleJuceChildren != nullptr) *visibleJuceChildren = context.seen;
    return context.hidden;
   #else
    juce::ignoreUnused(mainWindowHandle, visibleJuceChildren);
    return 0;
   #endif
}

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

