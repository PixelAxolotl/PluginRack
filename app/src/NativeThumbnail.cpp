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

} // namespace nativedisplay
