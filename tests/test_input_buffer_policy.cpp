#include <catch2/catch_test_macros.hpp>

#include <pluginrack/InputBufferPolicy.h>

TEST_CASE("Auto follows the render block so every open asks for the same period",
          "[latency][input]") {
    // The bug: Auto passed 0 to the driver, which picked 480 on one launch and
    // 600 on the next, and the read delay followed it (10.7 ms -> 13.2 ms).
    CHECK(pluginrack::inputBufferRequest(0, 480) == 480);
    CHECK(pluginrack::inputBufferRequest(0, 960) == 960);
}

TEST_CASE("an explicit input buffer always wins", "[latency][input]") {
    CHECK(pluginrack::inputBufferRequest(256, 960) == 256);
    CHECK(pluginrack::inputBufferRequest(128, 480) == 128);
}

TEST_CASE("no render block yet stays Auto", "[latency][input]") {
    CHECK(pluginrack::inputBufferRequest(0, 0) == 0);
    CHECK(pluginrack::inputBufferRequest(0, -1) == 0);
}
