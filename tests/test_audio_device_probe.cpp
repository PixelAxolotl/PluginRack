#include <catch2/catch_test_macros.hpp>

#include <pluginrack/AudioDeviceProbe.h>

TEST_CASE("a disabled probe cap is zero", "[device][probe]") {
    CHECK(pluginrack::wasapiProbeCapValue(false, 8) == 0);
    CHECK(pluginrack::wasapiProbeCapValue(false, 64) == 0);
}

TEST_CASE("an enabled probe cap is clamped to two through sixty-four", "[device][probe]") {
    CHECK(pluginrack::wasapiProbeCapValue(true, 2) == 2);
    CHECK(pluginrack::wasapiProbeCapValue(true, 8) == 8);
    CHECK(pluginrack::wasapiProbeCapValue(true, 64) == 64);
    CHECK(pluginrack::wasapiProbeCapValue(true, 0) == 2);
    CHECK(pluginrack::wasapiProbeCapValue(true, 1) == 2);
    CHECK(pluginrack::wasapiProbeCapValue(true, -4) == 2);
    CHECK(pluginrack::wasapiProbeCapValue(true, 65) == 64);
}
