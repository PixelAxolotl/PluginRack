#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "pluginrack/AudioBlock.h"
#include "pluginrack/EqEngine.h"
#include "pluginrack/DynamicsEffect.h"
#include "pluginrack/MultibandEffect.h"
#include "pluginrack/DeEsserEffect.h"
#include "pluginrack/LimiterEffect.h"
#include "pluginrack/InsertChain.h"
#include "pluginrack/Meters.h"
#include "pluginrack/PluginHost.h"
#include "pluginrack/Scene.h"
#include "pluginrack/SceneAvailability.h"

namespace pluginrack {

class MixerEngine {
public:
    MixerEngine();

    void setPluginFactory(IPluginFactory* factory) noexcept;
    // Scene-v6 opt-in for non-finite scrubbing. Must be set before prepare()
    // so the owned InsertChains inherit it; render() reads the same flag.
    void setScrubEnabled(bool enabled) noexcept { scrubEnabled_ = enabled; }
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
    // Non-finite samples replaced with silence inside the render path,
    // including those scrubbed by the per-strip insert chains.
    std::uint64_t scrubbedSamples() const;
    const MeterBank& meters() const noexcept;
    // EQ mutation API, indexed by scene-order position; out-of-range indexes
    // are ignored. setChannelEq/setOutputEq swap bands (bypass state kept);
    // setChannelEqBypassed/setOutputEqBypassed gate processing live without
    // re-prepare, and bypassing resets filter state so re-enabling starts
    // clean. These four are safe to call while the audio thread renders:
    // an internal mutex serializes them against render()'s EQ taps
    // (mirrors the app-side stateLock pattern). prepare() keeps its
    // existing contract: message thread with rendering stopped.
    void setChannelEq(int ch, const std::vector<EqBand>& bands);
    void setOutputEq(int out, const std::vector<EqBand>& bands);
    void setChannelEqBypassed(int ch, bool bypassed);
    void setOutputEqBypassed(int out, bool bypassed);

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
        // The live engine treats the two sides independently; a route that
        // only stored one level rendered a split send as a matched pair.
        float sendGainL = 0.0f;
        float sendGainR = 0.0f;
    };

    // Mutex and EQ state grouped behind one pointer so MixerEngine stays
    // move-constructible (the class is returned by value from prepare
    // helpers); exactly one owner sees the mutex, render() and the four
    // setters are its only users.
    struct EqState {
        std::mutex mutex;
        std::vector<EqEngine> channelEqs;
        std::vector<EqEngine> outputEqs;
        std::vector<char> channelEqEnabled;
        std::vector<char> outputEqEnabled;
        std::vector<std::vector<DynamicsProcessor>> channelEffects;
        std::vector<std::vector<MultibandProcessor>> channelMultiband;
        std::vector<std::vector<DeEsserProcessor>> channelDeEssers;
        std::vector<std::vector<LimiterProcessor>> channelLimiters;
        std::vector<std::vector<MultibandProcessor>> outputMultiband;
        std::vector<std::vector<DeEsserProcessor>> outputDeEssers;
        std::vector<std::vector<LimiterProcessor>> outputLimiters;
        std::vector<std::vector<MultibandEffect>> channelMultibandFx;
        std::vector<std::vector<DeEsserEffect>> channelDeEsserFx;
        std::vector<std::vector<LimiterEffect>> channelLimiterFx;
        std::vector<std::vector<MultibandEffect>> outputMultibandFx;
        std::vector<std::vector<DeEsserEffect>> outputDeEsserFx;
        std::vector<std::vector<LimiterEffect>> outputLimiterFx;
        std::vector<char> channelChainMode;
        std::vector<std::vector<ChainItem>> channelChains;
        std::vector<std::vector<ChainItem>> outputChains;
        std::vector<std::vector<DynamicsProcessor>> outputEffects;
    };

    std::vector<ChannelCache> channels_;
    std::vector<RouteCache> routes_;
    // Output fader/mute, applied after the output inserts like the app.
    std::vector<float> outputGains_;
    std::vector<char> outputMuted_;
    std::vector<float> scratch_;
    std::vector<float> accumulators_;
    std::vector<std::unique_ptr<InsertChain>> insertChains_;
    std::vector<std::unique_ptr<InsertChain>> outputInsertChains_;
    std::unique_ptr<EqState> eqState_;
    IPluginFactory* pluginFactory_ = nullptr;
    std::vector<char> stoppedChannels_;
    std::vector<char> disabledOutputs_;
    std::vector<std::uint32_t> channelIds_;
    std::vector<std::uint32_t> outputIds_;
    MeterBank meters_;
    std::uint64_t scrubbedSamples_ = 0;
    bool scrubEnabled_ = false;
    double sampleRate_ = 48000.0;
    std::size_t outputCount_ = 0;
    int maxBlockFrames_ = 0;
    bool anySolo_ = false;
};

} // namespace pluginrack
