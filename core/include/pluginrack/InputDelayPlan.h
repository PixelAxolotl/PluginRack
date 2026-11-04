#pragma once

#include <algorithm>

namespace pluginrack {

// The capture read delay: how much history the render thread keeps between the
// newest captured sample and the one it plays.
//
// Two numbers decide it and they are known at different times:
//   * the capture device's own buffer, known when the capture opens;
//   * the engine's render block, which belongs to the *clock* output and may
//     still be negotiating when a capture opens.
//
// Using a placeholder for the second one and never revisiting it is what made
// the delay differ between loads: the same setup came out short when the clock
// happened to be ready and long when it was not. The plan therefore separates
// "what we know now" from "the final answer", and `applyRenderBlock()` is the
// single place that settles it.
class InputDelayPlan {
public:
    struct Config {
        // Blocks of history to hold before the first read: 1 is the
        // low-latency choice, 2 the conservative one.
        int readDelayBlocks = 2;
        // CaptureReadCursor needs one block plus its look-ahead sample before it
        // can return a window; this is the jitter headroom on top of that.
        int headroomSamples = 32;
    };

    void open(int deviceBufferFrames, int provisionalRenderBlockFrames, Config config) {
        config_ = config;
        deviceBufferFrames_ = std::max(1, deviceBufferFrames);
        config_.readDelayBlocks = std::max(1, config_.readDelayBlocks);
        config_.headroomSamples = std::max(0, config_.headroomSamples);
        blockFrames_ = std::max(deviceBufferFrames_,
                                provisionalRenderBlockFrames > 0 ? provisionalRenderBlockFrames
                                                                 : deviceBufferFrames_);
        settle();
    }

    // The clock has finished negotiating. This is the authoritative block, and
    // it replaces any placeholder the open used.
    int applyRenderBlock(int renderBlockFrames) {
        blockFrames_ = std::max(deviceBufferFrames_, std::max(1, renderBlockFrames));
        settle();
        return delay_;
    }

    int blockFrames() const noexcept { return blockFrames_; }
    int delay() const noexcept { return delay_; }

    // The smallest delay this cursor can serve: one full block plus the
    // look-ahead sample, rounded up by the headroom.
    int floor() const noexcept { return blockFrames_ + config_.headroomSamples; }

private:
    void settle() { delay_ = std::max(floor(), blockFrames_ * config_.readDelayBlocks); }

    Config config_{};
    int deviceBufferFrames_ = 1;
    int blockFrames_ = 1;
    int delay_ = 0;
};

} // namespace pluginrack
