#pragma once

#include <algorithm>

namespace pluginrack {

// Which buffer size to ask a capture device for.
//
// A request of 0 means "Auto", which used to be passed to the driver untouched.
// Windows shared mode then decides the period itself, and it does not always
// decide the same way twice: the same input came back as 480 samples on one
// launch and 600 on the next. The read delay is derived from that period, so
// the input latency moved with it (10.7 ms, then 13.2 ms).
//
// Auto should mean "follow the engine": asking for the render block makes the
// capture period match the cadence the rest of the app runs at, so every open
// lands on the same number. An explicit user choice always wins.
inline int inputBufferRequest(int requestedBufferSize, int renderBlockFrames) {
    return requestedBufferSize > 0 ? requestedBufferSize : std::max(0, renderBlockFrames);
}

} // namespace pluginrack
