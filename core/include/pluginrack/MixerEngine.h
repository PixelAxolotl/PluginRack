#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pluginrack/AudioBlock.h"
#include "pluginrack/EqEngine.h"
#include "pluginrack/InsertChain.h"
#include "pluginrack/Meters.h"
#include "pluginrack/PluginHost.h"
#include "pluginrack/Scene.h"
#include "pluginrack/SceneAvailability.h"

namespace pluginrack {

class MixerEngine {
public:
    void setPluginFactory(IPluginFactory* factory) noexcept;
    // Returns false and fills *error when the scene is invalid or has no outputs.
    bool prepare(const Scene& scene, int maxBlockFrames, std::string* error);
    // Availability overload: stopped channels render silence and disabled
    // outputs are written as silence; the rest of the scene renders.
    bool prepare(const Scene& scene, const SceneAvailability& availability,
                 int maxBlockFrames, std::string* error);
    void render(std::span<const AudioBlock> channelInputs,
                std::span<AudioBlock> outputBlocks) noexcept;
    int channelCount() const noexcept;
    int outputCount() const noexcept;
    std::vector<std::uint32_t> failedPluginSlots() const;
    const MeterBank& meters() const noexcept;
    // Swaps EQ bands for the channel/output at the scene-order index;
    // out-of-range indexes are ignored. Message thread only: coefficients
    // are rebuilt inside EqEngine::setBands(). The scene's eqEnabled flag
    // captured by prepare() gates processing, so setting bands never
    // enables a bypassed strip.
    void setChannelEq(int ch, const std::vector<EqBand>& bands);
    void setOutputEq(int out, const std::vector<EqBand>& bands);

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
    std::vector<std::unique_ptr<InsertChain>> insertChains_;
    std::vector<std::unique_ptr<InsertChain>> outputInsertChains_;
    std::vector<EqEngine> channelEqs_;
    std::vector<EqEngine> outputEqs_;
    std::vector<char> channelEqEnabled_;
    std::vector<char> outputEqEnabled_;
    IPluginFactory* pluginFactory_ = nullptr;
    std::vector<char> stoppedChannels_;
    std::vector<char> disabledOutputs_;
    std::vector<std::uint32_t> channelIds_;
    std::vector<std::uint32_t> outputIds_;
    MeterBank meters_;
    double sampleRate_ = 48000.0;
    std::size_t outputCount_ = 0;
    int maxBlockFrames_ = 0;
    bool anySolo_ = false;
};

} // namespace pluginrack
