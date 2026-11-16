#include <catch2/catch_test_macros.hpp>

#include "pluginrack/DeviceTypePolicy.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

namespace {

using pluginrack::DeviceBackend;
using pluginrack::DeviceTypeNames;
using pluginrack::StreamRole;
using Order = std::vector<std::string_view>;

bool contains(const Order& order, std::string_view type) {
    return std::find(order.begin(), order.end(), type) != order.end();
}

bool hasDuplicates(const Order& order) {
    for (std::size_t i = 0; i < order.size(); ++i) {
        for (std::size_t j = i + 1; j < order.size(); ++j) {
            if (order[i] == order[j]) {
                return true;
            }
        }
    }
    return false;
}

std::size_t positionOf(const Order& order, std::string_view type) {
    return static_cast<std::size_t>(
        std::find(order.begin(), order.end(), type) - order.begin());
}

} // namespace

TEST_CASE("Clock probes exclusive mode first only when it is preferred") {
    const auto preferred = pluginrack::deviceTypeOrder(StreamRole::Clock, true, true);
    CHECK(preferred.front() == DeviceTypeNames::exclusive);

    const auto notPreferred = pluginrack::deviceTypeOrder(StreamRole::Clock, false, true);
    CHECK(notPreferred.front() != DeviceTypeNames::exclusive);
    // Exclusive is the clock's last resort when shared types are tried first.
    CHECK(notPreferred.back() == DeviceTypeNames::exclusive);
}

TEST_CASE("Clock reaches ASIO before the shared types when the build has it") {
    const auto order = pluginrack::deviceTypeOrder(StreamRole::Clock, false, true);
    CHECK(positionOf(order, DeviceTypeNames::asio) <
          positionOf(order, DeviceTypeNames::sharedLowLatency));
    CHECK(positionOf(order, DeviceTypeNames::sharedLowLatency) <
          positionOf(order, DeviceTypeNames::shared));
}

TEST_CASE("Captures and extra outputs stay on shareable types") {
    for (const auto role : {StreamRole::Output, StreamRole::Input}) {
        for (const bool preferExclusive : {false, true}) {
            const auto order = pluginrack::deviceTypeOrder(role, preferExclusive, true);
            CHECK_FALSE(contains(order, DeviceTypeNames::exclusive));
            // Shared WASAPI is the one every endpoint can open, so it comes first.
            CHECK(order.front() == DeviceTypeNames::shared);
            CHECK(positionOf(order, DeviceTypeNames::sharedLowLatency) <
                  positionOf(order, DeviceTypeNames::asio));
        }
    }
}

TEST_CASE("ASIO is absent when the build does not provide it") {
    for (const auto role : {StreamRole::Clock, StreamRole::Output, StreamRole::Input}) {
        for (const bool preferExclusive : {false, true}) {
            CHECK_FALSE(contains(pluginrack::deviceTypeOrder(role, preferExclusive, false),
                                 DeviceTypeNames::asio));
        }
    }
}

TEST_CASE("Every produced order is non-empty and free of duplicates") {
    for (const auto role : {StreamRole::Clock, StreamRole::Output, StreamRole::Input}) {
        for (const bool preferExclusive : {false, true}) {
            for (const bool asioAvailable : {false, true}) {
                const auto order = pluginrack::deviceTypeOrder(role, preferExclusive, asioAvailable);
                CHECK_FALSE(order.empty());
                CHECK_FALSE(hasDuplicates(order));
            }
        }
    }
}

TEST_CASE("Every probeable type maps to one label and one persisted mode") {
    // The opener, the strips and the settings page each need a name for the
    // backend; a type added to the policy without a mapping would show a raw
    // registered name and persist an empty mode.
    const std::string_view types[] = {
        DeviceTypeNames::shared,
        DeviceTypeNames::sharedLowLatency,
        DeviceTypeNames::exclusive,
        DeviceTypeNames::asio,
    };
    for (const auto type : types) {
        const auto backend = pluginrack::deviceBackendOf(type);
        REQUIRE(backend != DeviceBackend::Unknown);
        CHECK_FALSE(pluginrack::deviceBackendTag(backend).empty());
        CHECK_FALSE(pluginrack::deviceModeId(backend).empty());
    }
    CHECK(pluginrack::deviceBackendOf(DeviceTypeNames::exclusive) == DeviceBackend::Exclusive);
    CHECK(pluginrack::deviceBackendOf(DeviceTypeNames::sharedLowLatency) ==
          DeviceBackend::SharedLowLatency);
    // Anything the policy does not know keeps its own name and does not claim a mode.
    CHECK(pluginrack::deviceBackendOf("DirectSound") == DeviceBackend::Unknown);
    CHECK(pluginrack::deviceBackendTag(DeviceBackend::Unknown).empty());
    CHECK(pluginrack::deviceModeId(DeviceBackend::Unknown).empty());
}
