#include "pluginrack/InsertChain.h"

#include <string>

namespace pluginrack {

InsertChain::InsertChain(const std::vector<InsertSlot>& slots, IPluginFactory& factory,
                         double sampleRate, int maxBlockFrames) {
    entries_.reserve(slots.size());
    for (const InsertSlot& slot : slots) {
        if (slot.bypassed) {
            entries_.push_back(Entry{slot.id, nullptr});
            continue;
        }
        std::string error;
        std::unique_ptr<IPluginInstance> instance = factory.create(slot.pluginPath, &error);
        if (instance == nullptr) {
            failed_.push_back(slot.id);
            entries_.push_back(Entry{slot.id, nullptr});
            continue;
        }
        instance->prepare(sampleRate, maxBlockFrames);
        entries_.push_back(Entry{slot.id, std::move(instance)});
    }
}

void InsertChain::process(AudioBlock& block) noexcept {
    for (Entry& entry : entries_) {
        if (entry.instance != nullptr) {
            entry.instance->process(block);
        }
    }
}

std::vector<std::uint32_t> InsertChain::failedSlots() const {
    return failed_;
}

} // namespace pluginrack
