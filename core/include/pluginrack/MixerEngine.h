#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "pluginrack/AudioBlock.h"
#include "pluginrack/Scene.h"

namespace pluginrack {

class MixerEngine {
public:
    // Returns false and fills *error when the scene is invalid or has no outputs.
    bool prepare(const Scene& scene, int maxBlockFrames, std::string* error);
    void render(std::span<const AudioBlock> channelInputs,
                std::span<AudioBlock> outputBlocks) noexcept;
    int channelCount() const noexcept;
    int outputCount() const noexcept;

private:
    struct ChannelCache {
        float gain = 0.0f;
        float panLeft = 0.0f;
        float panRight = 0.0f;
        bool muted = false;
        bool soloed = false;
    };

    struct RouteCache {
        std::size_t channelIndex = 0;
        std::size_t outputIndex = 0;
        float sendGain = 0.0f;
    };

    std::vector<ChannelCache> channels_;
    std::vector<RouteCache> routes_;
    std::vector<float> scratch_;
    std::vector<float> accumulators_;
    std::size_t outputCount_ = 0;
    int maxBlockFrames_ = 0;
    bool anySolo_ = false;
};

} // namespace pluginrack
