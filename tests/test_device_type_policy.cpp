#include <catch2/catch_test_macros.hpp>

#include "pluginrack/DeviceTypePolicy.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Order = std::vector<std::string_view>;

// Renders "a, b, c" so the expected order is readable inside the assertion.
std::string render(const Order& order) {
    std::string text;
    for (const auto type : order) {
        if (text.empty() == false) {
            text += ", ";
        }
        text += type;
    }
    return text;
}

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

} // namespace

TEST_CASE("Clock stream probes exclusive mode first when it is preferred") {
    using pluginrack::StreamRole;
    using pluginrack::deviceTypeOrder;

    CHECK(render(deviceTypeOrder(StreamRole::Clock, true, true))
          == "Windows Audio (Exclusive Mode), ASIO, Windows Audio (Low Latency Mode), "
             "Windows Audio");
    CHECK(render(deviceTypeOrder(StreamRole::Clock, true, false))
          == "Windows Audio (Exclusive Mode), Windows Audio (Low Latency Mode), "
             "Windows Audio");
}

TEST_CASE("Clock stream prefers ASIO and low latency when exclusive is not preferred") {
    using pluginrack::StreamRole;
    using pluginrack::deviceTypeOrder;

    CHECK(render(deviceTypeOrder(StreamRole::Clock, false, true))
          == "ASIO, Windows Audio (Low Latency Mode), Windows Audio, "
             "Windows Audio (Exclusive Mode)");
    CHECK(render(deviceTypeOrder(StreamRole::Clock, false, false))
          == "Windows Audio (Low Latency Mode), Windows Audio, "
             "Windows Audio (Exclusive Mode)");
}

TEST_CASE("Output stream keeps shared types first and never uses exclusive") {
    using pluginrack::StreamRole;
    using pluginrack::deviceTypeOrder;

    CHECK(render(deviceTypeOrder(StreamRole::Output, false, true))
          == "Windows Audio, Windows Audio (Low Latency Mode), ASIO");
    CHECK(render(deviceTypeOrder(StreamRole::Output, false, false))
          == "Windows Audio, Windows Audio (Low Latency Mode)");

    // preferExclusive must not leak into a non-clock role.
    CHECK(render(deviceTypeOrder(StreamRole::Output, true, true))
          == "Windows Audio, Windows Audio (Low Latency Mode), ASIO");
    CHECK(render(deviceTypeOrder(StreamRole::Output, true, false))
          == "Windows Audio, Windows Audio (Low Latency Mode)");
    CHECK_FALSE(contains(deviceTypeOrder(StreamRole::Output, true, true),
                         pluginrack::DeviceTypeNames::exclusive));
}

TEST_CASE("Input stream keeps shared types first and never uses exclusive") {
    using pluginrack::StreamRole;
    using pluginrack::deviceTypeOrder;

    CHECK(render(deviceTypeOrder(StreamRole::Input, false, true))
          == "Windows Audio, Windows Audio (Low Latency Mode), ASIO");
    CHECK(render(deviceTypeOrder(StreamRole::Input, false, false))
          == "Windows Audio, Windows Audio (Low Latency Mode)");

    CHECK(render(deviceTypeOrder(StreamRole::Input, true, true))
          == "Windows Audio, Windows Audio (Low Latency Mode), ASIO");
    CHECK(render(deviceTypeOrder(StreamRole::Input, true, false))
          == "Windows Audio, Windows Audio (Low Latency Mode)");
    CHECK_FALSE(contains(deviceTypeOrder(StreamRole::Input, true, true),
                         pluginrack::DeviceTypeNames::exclusive));
}

TEST_CASE("ASIO is absent when the build does not provide it") {
    using pluginrack::StreamRole;
    using pluginrack::deviceTypeOrder;

    CHECK_FALSE(contains(deviceTypeOrder(StreamRole::Clock, false, false),
                         pluginrack::DeviceTypeNames::asio));
    CHECK_FALSE(contains(deviceTypeOrder(StreamRole::Clock, true, false),
                         pluginrack::DeviceTypeNames::asio));
    CHECK_FALSE(contains(deviceTypeOrder(StreamRole::Output, false, false),
                         pluginrack::DeviceTypeNames::asio));
    CHECK_FALSE(contains(deviceTypeOrder(StreamRole::Input, false, false),
                         pluginrack::DeviceTypeNames::asio));
}

TEST_CASE("Every produced order is non-empty and free of duplicates") {
    using pluginrack::StreamRole;
    using pluginrack::deviceTypeOrder;

    const StreamRole roles[] = {StreamRole::Clock, StreamRole::Output, StreamRole::Input};
    for (const auto role : roles) {
        for (const bool preferExclusive : {false, true}) {
            for (const bool asioAvailable : {false, true}) {
                const auto order = deviceTypeOrder(role, preferExclusive, asioAvailable);
                CHECK_FALSE(order.empty());
                CHECK_FALSE(hasDuplicates(order));
            }
        }
    }
}

TEST_CASE("ASIO-only machine still tries the shared types for Output and Input") {
    using pluginrack::StreamRole;
    using pluginrack::DeviceTypeNames;
    using pluginrack::deviceTypeOrder;

    const auto output = deviceTypeOrder(StreamRole::Output, false, true);
    // The shared endpoint is the one we can always open, so it is probed first and
    // ASIO only gets a chance after it.
    CHECK(output.front() == DeviceTypeNames::shared);
    CHECK(contains(output, DeviceTypeNames::sharedLowLatency));
    CHECK(contains(output, DeviceTypeNames::asio));

    const auto input = deviceTypeOrder(StreamRole::Input, false, true);
    CHECK(input.front() == DeviceTypeNames::shared);
    CHECK(contains(input, DeviceTypeNames::sharedLowLatency));
    CHECK(contains(input, DeviceTypeNames::asio));
}
