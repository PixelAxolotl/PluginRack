#pragma once

#include <algorithm>

namespace pluginrack {

// Split one device block into equal render slices. The layout is pure
// arithmetic so the audio thread can plan a block without allocating. A
// power-of-two block gets power-of-two slices (the VST3 block-size hints stay
// friendly); a non-power-of-two block puts the remainder in the last slice.
struct SliceLayout {
    int blockFrames = 0;
    int count = 0;
    int sliceFrames = 0;
    int remainderFrames = 0;

    int framesAt(int index) const noexcept {
        if (index < 0 || index >= count) return 0;
        return index == count - 1 ? sliceFrames + remainderFrames : sliceFrames;
    }

    int offsetAt(int index) const noexcept {
        if (index < 0) return 0;
        return index * sliceFrames;
    }

    int totalFrames() const noexcept { return sliceFrames * count + remainderFrames; }
};

inline SliceLayout planSlices(int blockFrames, int maxSlices = 4) noexcept {
    SliceLayout layout;
    layout.blockFrames = blockFrames > 0 ? blockFrames : 0;
    if (layout.blockFrames <= 0) return layout;

    int slices = maxSlices < 1 ? 1 : maxSlices;
    slices = std::min(slices, layout.blockFrames);
    layout.count = slices;
    layout.sliceFrames = layout.blockFrames / slices;
    layout.remainderFrames = layout.blockFrames - layout.sliceFrames * slices;
    return layout;
}

// The render callback may spend at most this share of one block period before
// the remaining slices are written as silence. Keeping headroom below 100 %
// absorbs the scheduler jitter between the deadline check and the actual
// driver callback.
inline double deadlineBudgetMs(int blockFrames, double sampleRate,
                               double budgetFraction = 0.8) noexcept {
    if (blockFrames <= 0 || sampleRate <= 0.0 || budgetFraction <= 0.0) return 0.0;
    return static_cast<double>(blockFrames) / sampleRate * 1000.0 * budgetFraction;
}

inline bool deadlineExceededMs(double elapsedMs, double budgetMs) noexcept {
    return budgetMs > 0.0 && elapsedMs >= budgetMs;
}

} // namespace pluginrack
