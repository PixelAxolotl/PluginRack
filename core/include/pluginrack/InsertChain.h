#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "pluginrack/AudioBlock.h"
#include "pluginrack/PluginHost.h"
#include "pluginrack/Scene.h"

namespace pluginrack {

// Per-channel VST insert chain. Construction may allocate; process() must not.
class InsertChain {
public:
    // scrubEnabled is the scene-v6 opt-in. Off by default so a pre-v6 save
    // keeps the historical pass-through behaviour bit for bit.
    InsertChain(const std::vector<InsertSlot>& slots, IPluginFactory& factory,
                double sampleRate, int maxBlockFrames, bool scrubEnabled = false);
    void process(AudioBlock& block) noexcept; // allocation-free
    std::vector<std::uint32_t> failedSlots() const; // slot ids that failed to load or process
    // Non-finite samples replaced with silence at the insert boundaries.
    // Cumulative for the lifetime of the chain; the delta per block is what
    // telemetry reports.
    std::uint64_t scrubbedSamples() const noexcept { return scrubbedSamples_; }

private:
    struct Entry {
        std::uint32_t id = 0;
        std::unique_ptr<IPluginInstance> instance;
        bool failed = false;
    };

    std::vector<Entry> entries_;
    std::vector<std::uint32_t> failed_;
    std::uint64_t scrubbedSamples_ = 0;
    bool scrubEnabled_ = false;
};

} // namespace pluginrack
