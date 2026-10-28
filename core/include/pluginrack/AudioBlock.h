#pragma once

namespace pluginrack {

struct AudioBlock {
    float* const* channels;
    int numChannels;
    int numFrames;
};

} // namespace pluginrack
