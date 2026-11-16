#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace effectedit {

// Replaces the stored effect with the edited copy, keeping the bypass state the
// strip already had unless the window's own bypass toggle produced the edit.
// Every editor window hands back a whole effect struct, so without this rule a
// knob turn would silently undo a card-level bypass.
template <typename Effect>
void applyEdit(std::vector<Effect>& effects, const Effect& edited, bool bypassChanged) {
    const auto found = std::find_if(effects.begin(), effects.end(),
                                    [&edited](const Effect& candidate) {
                                        return candidate.id == edited.id;
                                    });
    if (found == effects.end()) return;
    const bool bypassed = found->bypassed;
    *found = edited;
    if (!bypassChanged) found->bypassed = bypassed;
}

// Used by the chain-row bypass switch: the row carries the effect id, and only
// that effect changes so the rest of the strip keeps its own switches.
template <typename Effect>
void setBypass(std::vector<Effect>& effects, std::uint32_t id, bool bypassed) {
    for (auto& effect : effects)
        if (effect.id == id) effect.bypassed = bypassed;
}

} // namespace effectedit
