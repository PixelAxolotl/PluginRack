#include <algorithm>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "pluginrack/ChainOrder.h"

namespace {

using pluginrack::ChainItem;
using pluginrack::ChainItemType;
using pluginrack::ChainMove;

ChainItem eqItem() { return {ChainItemType::Eq, 0}; }
ChainItem compItem() { return {ChainItemType::Dynamics, 1}; }
ChainItem gateItem() { return {ChainItemType::Dynamics, 2}; }
ChainItem vstItem(std::uint32_t id) { return {ChainItemType::Insert, id}; }

std::vector<pluginrack::DynamicsEffect> effects() {
    return pluginrack::defaultDynamicsEffects();
}

// What a fresh strip shows: EQ, the paired Dynamics card, then two VSTs.
std::vector<ChainItem> freshChain() {
    return {eqItem(), compItem(), gateItem(), vstItem(10), vstItem(11)};
}

// Applies planned moves the way the engine does, through the same function.
void applyMoves(std::vector<ChainItem>& chain, const std::vector<ChainMove>& moves) {
    for (const auto& move : moves) {
        REQUIRE(std::find(chain.begin(), chain.end(), move.item) != chain.end());
        pluginrack::moveChainItem(chain, move.item, move.targetIndex);
    }
}

} // namespace

TEST_CASE("chain rows pair the dynamics effects into one card") {
    const auto chain = freshChain();
    const auto rows = pluginrack::buildChainRows(chain, effects());
    REQUIRE(rows.size() == 4);
    CHECK(rows[0] == std::vector<int>{0});
    CHECK(rows[1] == std::vector<int>{1, 2});
    CHECK(rows[2] == std::vector<int>{3});
    CHECK(rows[3] == std::vector<int>{4});

    CHECK(pluginrack::chainIndexForBoundary(rows, 0, 5) == 0);
    CHECK(pluginrack::chainIndexForBoundary(rows, 1, 5) == 1);
    CHECK(pluginrack::chainIndexForBoundary(rows, 2, 5) == 3);
    CHECK(pluginrack::chainIndexForBoundary(rows, 3, 5) == 4);
    CHECK(pluginrack::chainIndexForBoundary(rows, 4, 5) == 5);
}

TEST_CASE("a downward drop lands on the boundary it was dropped on") {
    auto chain = freshChain();
    const auto rows = pluginrack::buildChainRows(chain, effects());
    // EQ dropped before the first VST row (past the Dynamics card). The engine
    // removes EQ first, so the target index must not stay on the pre-move one.
    const auto moves = pluginrack::planChainCardMoves(chain, rows, 2, {eqItem()});
    REQUIRE(moves.size() == 1);
    CHECK(moves[0].targetIndex == 2);
    applyMoves(chain, moves);
    const auto expected = std::vector<ChainItem>{compItem(), gateItem(), eqItem(), vstItem(10),
                                                 vstItem(11)};
    CHECK(chain == expected);
}

TEST_CASE("a drop on the dragged card's own row does nothing") {
    const auto chain = freshChain();
    const auto rows = pluginrack::buildChainRows(chain, effects());

    CHECK(pluginrack::planChainCardMoves(chain, rows, 1, {eqItem()}).empty());
    // A paired card's own top boundary must not shift it either.
    CHECK(pluginrack::planChainCardMoves(chain, rows, 1, {compItem(), gateItem()}).empty());
}

TEST_CASE("an upward drop and an end drop keep their exact slots") {
    const auto chain = freshChain();
    const auto rows = pluginrack::buildChainRows(chain, effects());

    auto up = chain;
    const auto upMoves = pluginrack::planChainCardMoves(up, rows, 0, {vstItem(11)});
    REQUIRE(upMoves.size() == 1);
    applyMoves(up, upMoves);
    const auto expectedUp = std::vector<ChainItem>{vstItem(11), eqItem(), compItem(), gateItem(),
                                                   vstItem(10)};
    CHECK(up == expectedUp);

    auto end = chain;
    const auto endMoves = pluginrack::planChainCardMoves(end, rows, 4, {eqItem()});
    REQUIRE(endMoves.size() == 1);
    applyMoves(end, endMoves);
    const auto expectedEnd = std::vector<ChainItem>{compItem(), gateItem(), vstItem(10),
                                                    vstItem(11), eqItem()};
    CHECK(end == expectedEnd);
}

TEST_CASE("a paired card drops below a VST without splitting") {
    auto chain = freshChain();
    const auto rows = pluginrack::buildChainRows(chain, effects());
    // Dynamics dropped before the second VST (boundary 3).
    const auto moves = pluginrack::planChainCardMoves(chain, rows, 3, {compItem(), gateItem()});
    REQUIRE(moves.size() == 2);
    applyMoves(chain, moves);
    const auto expected = std::vector<ChainItem>{eqItem(), vstItem(10), compItem(), gateItem(),
                                                 vstItem(11)};
    CHECK(chain == expected);
}

TEST_CASE("a paired card moves to the top as one block") {
    auto chain = freshChain();
    const auto rows = pluginrack::buildChainRows(chain, effects());
    const auto moves = pluginrack::planChainCardMoves(chain, rows, 0, {compItem(), gateItem()});
    REQUIRE(moves.size() == 2);
    applyMoves(chain, moves);
    const auto expected = std::vector<ChainItem>{compItem(), gateItem(), eqItem(), vstItem(10),
                                                 vstItem(11)};
    CHECK(chain == expected);
}

TEST_CASE("an item tap resolves to the first chain row with its id") {
    // Tap::Kind::Item carries only an id, and every built-in family mints its
    // own id 1. The identity rule is the first row whose id matches: that is
    // the row the engine pushes into the ring and the row whose delay it
    // bills, so content and latency cannot come from different rows.
    const std::vector<ChainItem> chain{{ChainItemType::Dynamics, 1},
                                       {ChainItemType::Eq, 0},
                                       {ChainItemType::Limiter, 1}};
    CHECK(pluginrack::firstChainItemIndex(chain, 1) == 0);
    CHECK(pluginrack::firstChainItemIndex(chain, 0) == 1);
    CHECK(pluginrack::firstChainItemIndex(chain, 7) == -1);

    // The rule follows chain order, not the family: once the limiter row is
    // first, id 1 is the limiter and the dynamics row is unreachable.
    const std::vector<ChainItem> flipped{{ChainItemType::Limiter, 1},
                                         {ChainItemType::Dynamics, 1}};
    CHECK(pluginrack::firstChainItemIndex(flipped, 1) == 0);
    CHECK(pluginrack::firstChainItemIndex(std::vector<ChainItem>{}, 1) == -1);
}
