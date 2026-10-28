#include "pluginrack/InsertChain.h"

#include <string>

namespace pluginrack {

InsertChain::InsertChain(const std::vector<InsertSlot>& slots, IPluginFactory& factory,
                         double sampleRate, int maxBlockFrames) {
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
            if (entry.instance->process(block) == ProcessResult::Failed) {
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
