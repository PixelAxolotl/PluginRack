#pragma once

#include <algorithm>
#include <span>

namespace pluginrack {

// Which buffer size to end up at for a stream.
//
// "Auto" used to mean "ask the driver for nothing and keep whatever it handed
// back". That is not a decision the app can live with: with the same device,
// Windows returned 960 samples on one launch and 480 on the next, so the clock
// period — and every latency derived from it — moved between loads.
//
// Auto now means "the smallest this endpoint says it supports", which is both
// the lowest value available and stable for a given device.
inline int smallestSupportedBufferSize(std::span<const int> availableSizes,
                                       int currentBufferSize, int requestedBufferSize) {
    if (requestedBufferSize > 0) return requestedBufferSize;
    int smallest = 0;
    for (const int size : availableSizes) {
        if (size <= 0) continue;
        if (smallest == 0 || size < smallest) smallest = size;
    }
    return smallest > 0 ? smallest : currentBufferSize;
}

} // namespace pluginrack
