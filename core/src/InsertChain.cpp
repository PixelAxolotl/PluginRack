#include "pluginrack/InsertChain.h"

#include <string>

#include "pluginrack/SampleScrub.h"

namespace pluginrack {

InsertChain::InsertChain(const std::vector<InsertSlot>& slots, IPluginFactory& factory,
                         double sampleRate, int maxBlockFrames, bool scrubEnabled)
    : scrubEnabled_(scrubEnabled) {
    entries_.reserve(slots.size());
    // Reserve for every slot so recording a processing failure cannot allocate.
    failed_.reserve(slots.size());
    for (const InsertSlot& slot : slots) {
        if (slot.bypassed) {
            entries_.push_back(Entry{slot.id, nullptr, false});
            continue;
        }
        std::string error;
        std::unique_ptr<IPluginInstance> instance = factory.create(slot.pluginPath, &error);
        if (instance == nullptr) {
            failed_.push_back(slot.id);
            entries_.push_back(Entry{slot.id, nullptr, true});
            continue;
        }
        instance->prepare(sampleRate, maxBlockFrames);
        entries_.push_back(Entry{slot.id, std::move(instance), false});
    }
}

void InsertChain::process(AudioBlock& block) noexcept {
    for (Entry& entry : entries_) {
        if (entry.instance != nullptr && !entry.failed) {
            const bool failed = entry.instance->process(block) == ProcessResult::Failed;
            // Scrub even a failed render: the plugin may have written NaNs
            // before it reported failure, and those must not reach the next
            // insert or the mix bus. The guard is the v6 opt-in.
            if (scrubEnabled_) {
                scrubbedSamples_ += static_cast<std::uint64_t>(
                    scrubNonFinite(block.channels, block.numChannels, block.numFrames));
            }
            if (failed) {
                entry.failed = true;
                failed_.push_back(entry.id);
            }
        }
    }
}

std::vector<std::uint32_t> InsertChain::failedSlots() const {
    return failed_;
}

} // namespace pluginrack
