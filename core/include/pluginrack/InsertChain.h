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
    InsertChain(const std::vector<InsertSlot>& slots, IPluginFactory& factory,
                double sampleRate, int maxBlockFrames);
    void process(AudioBlock& block) noexcept; // allocation-free
    std::vector<std::uint32_t> failedSlots() const; // slot ids that failed to load

private:
    struct Entry {
        std::uint32_t id = 0;
        std::unique_ptr<IPluginInstance> instance;
    };

    std::vector<Entry> entries_;
    std::vector<std::uint32_t> failed_;
};

} // namespace pluginrack
