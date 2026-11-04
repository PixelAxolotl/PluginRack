#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "pluginrack/Scene.h"

namespace pluginrack {

// The built-in rows as every view draws them: the compressor and the gate share
// one "Dynamics" card (they are one editor and one gain-stage family), while
// the EQ keeps a row of its own. Each entry lists the chain indices that row
// covers, in chain order, so a drop can never split a card.
inline std::vector<std::vector<int>> buildChainRows(
        const std::vector<ChainItem>& chain, const std::vector<DynamicsEffect>& effects) {
    const int count = static_cast<int>(chain.size());
    int compressorIndex = -1;
    int gateIndex = -1;
    for (int i = 0; i < count; ++i) {
        const auto& item = chain[static_cast<std::size_t>(i)];
        if (item.type != ChainItemType::Dynamics) continue;
        if (compressorIndex >= 0 && gateIndex >= 0) continue;
        // A scene keeps the effect ids it was saved with, so resolve by id and
        // fall back to the chain row's own convention (row 1 is the
        // compressor) before calling the effect a gate.
        const auto effect = std::find_if(effects.begin(), effects.end(),
                                         [&item](const auto& candidate) {
                                             return candidate.id == item.id;
                                         });
        const bool isCompressor =
            effect != effects.end() ? effect->type == DynamicsEffectType::Compressor
                                    : item.id == 1;
        if (isCompressor) {
            if (compressorIndex < 0) compressorIndex = i;
        } else if (gateIndex < 0) {
            gateIndex = i;
        }
    }

    std::vector<std::vector<int>> rows;
    std::vector<bool> claimed(static_cast<std::size_t>(count), false);
    for (int i = 0; i < count; ++i) {
        if (claimed[static_cast<std::size_t>(i)]) continue;
        int partner = -1;
        if (i == compressorIndex && gateIndex > i) partner = gateIndex;
        else if (i == gateIndex && compressorIndex > i) partner = compressorIndex;
        if (partner >= 0) {
            rows.push_back({ i, partner });
            claimed[static_cast<std::size_t>(partner)] = true;
        } else {
            rows.push_back({ i });
        }
        claimed[static_cast<std::size_t>(i)] = true;
    }
    return rows;
}

// Chain index a drop on a row boundary lands at. Rows do not map one-to-one to
// chain entries once a card covers two of them.
inline int chainIndexForBoundary(const std::vector<std::vector<int>>& rows, int row,
                                 int chainSize) {
    if (row < 0 || row >= static_cast<int>(rows.size())) return chainSize;
    const auto& indices = rows[static_cast<std::size_t>(row)];
    return indices.empty() ? chainSize : indices.front();
}

// Which chain entry of a paired card is the compressor. Both are Dynamics
// rows, so the effect type decides; a scene whose saved ids no longer match
// falls back to the row's own convention (id 1 is the compressor).
inline int compressorIndexInRow(const std::vector<ChainItem>& chain,
                                const std::vector<DynamicsEffect>& effects,
                                const std::vector<int>& row) {
    for (const int index : row) {
        const auto& item = chain[static_cast<std::size_t>(index)];
        const auto effect = std::find_if(effects.begin(), effects.end(),
                                         [&item](const auto& candidate) {
                                             return candidate.id == item.id;
                                         });
        const bool isCompressor =
            effect != effects.end() ? effect->type == DynamicsEffectType::Compressor
                                    : item.id == 1;
        if (isCompressor) return index;
    }
    return row.empty() ? -1 : row.front();
}

// One step of a chain-card move: the engine erases `item` and then inserts it
// at `targetIndex`, which is taken in the list as it stands after the erase.
struct ChainMove {
    ChainItem item;
    int targetIndex = 0;
};

// Engine insertion index that lands `anchor` on `boundary` (an index into the
// visible rows, 0..rows.size()). The engine removes the item before inserting
// it, so a boundary below the item's own slot resolves one place too low and
// must be pulled back up.
inline int chainDropInsertIndex(const std::vector<ChainItem>& chain,
                                const std::vector<std::vector<int>>& rows, int boundary,
                                ChainItem anchor) {
    const int target = chainIndexForBoundary(rows, boundary, static_cast<int>(chain.size()));
    const auto found = std::find(chain.begin(), chain.end(), anchor);
    if (found == chain.end()) return target;
    const int current = static_cast<int>(std::distance(chain.begin(), found));
    return current < target ? target - 1 : target;
}

// Where a follower has to go so it ends up immediately after `anchor`. Removing
// a follower that sits above the anchor shifts the anchor up first.
inline int chainInsertAfterIndex(const std::vector<ChainItem>& chain, ChainItem anchor,
                                 ChainItem item) {
    const auto anchorIt = std::find(chain.begin(), chain.end(), anchor);
    const auto itemIt = std::find(chain.begin(), chain.end(), item);
    if (anchorIt == chain.end() || itemIt == chain.end()) return -1;
    const int anchorIndex = static_cast<int>(std::distance(chain.begin(), anchorIt));
    const int itemIndex = static_cast<int>(std::distance(chain.begin(), itemIt));
    const int anchorAfterErase = itemIndex < anchorIndex ? anchorIndex - 1 : anchorIndex;
    return anchorAfterErase + 1;
}

// The engine calls one card move needs, in order. `chain` is taken by value:
// every planned move is applied to that copy, so each returned index is exactly
// what the engine will see when it applies the same steps.
inline std::vector<ChainMove> planChainCardMoves(std::vector<ChainItem> chain,
                                                 const std::vector<std::vector<int>>& rows,
                                                 int boundary,
                                                 const std::vector<ChainItem>& cardItems) {
    std::vector<ChainMove> moves;
    if (cardItems.empty()) return moves;
    for (std::size_t k = 0; k < cardItems.size(); ++k) {
        const auto found = std::find(chain.begin(), chain.end(), cardItems[k]);
        if (found == chain.end()) continue;
        const int target = k == 0
                               ? chainDropInsertIndex(chain, rows, boundary, cardItems[k])
                               : chainInsertAfterIndex(chain, cardItems[k - 1], cardItems[k]);
        if (target < 0) continue;
        const int current = static_cast<int>(std::distance(chain.begin(), found));
        const int index = std::clamp(target, 0, static_cast<int>(chain.size()));
        if (index == current) continue;
        const auto value = *found;
        chain.erase(found);
        chain.insert(chain.begin() + index, value);
        moves.push_back({ cardItems[k], index });
    }
    return moves;
}

} // namespace pluginrack
