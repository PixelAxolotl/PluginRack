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
        float sendGain = 0.0f;
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
        std::vector<std::vector<DynamicsProcessor>> outputEffects;
    };

    std::vector<ChannelCache> channels_;
    std::vector<RouteCache> routes_;
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
    double sampleRate_ = 48000.0;
    std::size_t outputCount_ = 0;
    int maxBlockFrames_ = 0;
    bool anySolo_ = false;
};

} // namespace pluginrack
