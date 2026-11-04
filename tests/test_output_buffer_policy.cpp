#include <catch2/catch_test_macros.hpp>

#include <pluginrack/OutputBufferPolicy.h>

namespace {

const int kSharedSizes[] = {480, 960, 1440};

} // namespace

TEST_CASE("Auto picks the smallest size the endpoint advertises", "[latency][output]") {
    // The bug: Auto kept whatever the driver defaulted to, which was 960 on one
    // launch and 480 on the next for the same device.
    CHECK(pluginrack::smallestSupportedBufferSize(kSharedSizes, 960, 0) == 480);
    CHECK(pluginrack::smallestSupportedBufferSize(kSharedSizes, 480, 0) == 480);

    const int onlyBig[] = {960};
    CHECK(pluginrack::smallestSupportedBufferSize(onlyBig, 960, 0) == 960);
}

TEST_CASE("an explicit request is never overridden", "[latency][output]") {
    CHECK(pluginrack::smallestSupportedBufferSize(kSharedSizes, 960, 256) == 256);
    CHECK(pluginrack::smallestSupportedBufferSize(kSharedSizes, 960, 1024) == 1024);
}

TEST_CASE("a device that publishes nothing keeps its current size", "[latency][output]") {
    CHECK(pluginrack::smallestSupportedBufferSize({}, 600, 0) == 600);
    const int invalid[] = {0, -1};
    CHECK(pluginrack::smallestSupportedBufferSize(invalid, 600, 0) == 600);
}
