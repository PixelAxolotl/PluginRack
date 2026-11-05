#include <catch2/catch_test_macros.hpp>

#include <pluginrack/InputFollowPolicy.h>

namespace {

const std::vector<std::string> kAsio{"Focusrite USB ASIO", "MOTU Audio ASIO"};

} // namespace

TEST_CASE("an input on the same interface follows the clock onto ASIO",
          "[latency][asio][duplex]") {
    // The bug: after moving the clock to ASIO the strip still pointed at
    // "Speakers (Focusrite USB Audio)", so duplex never engaged and the input
    // kept its own latency until the user forced a full reload.
    const std::vector<std::pair<std::uint32_t, std::string>> inputs{
        {1, "Speakers (Focusrite USB Audio)"},
        {2, "Microphone (MOTU Audio Interface)"},
        {3, "None - silent"},
        {4, ""},
    };
    const auto moved = pluginrack::inputsToRetargetForClock(inputs, "Focusrite USB ASIO", kAsio);
    REQUIRE(moved.size() == 1);
    CHECK(moved.front() == 1);
}

TEST_CASE("an input already on the clock device is left alone", "[latency][asio][duplex]") {
    const std::vector<std::pair<std::uint32_t, std::string>> inputs{
        {1, "Focusrite USB ASIO"},
    };
    CHECK(pluginrack::inputsToRetargetForClock(inputs, "Focusrite USB ASIO", kAsio).empty());
}

TEST_CASE("a different interface is never dragged along", "[latency][asio][duplex]") {
    const std::vector<std::pair<std::uint32_t, std::string>> inputs{
        {1, "Microphone (MOTU Audio Interface)"},
        {2, "Line In (Realtek High Definition Audio)"},
    };
    CHECK(pluginrack::inputsToRetargetForClock(inputs, "Focusrite USB ASIO", kAsio).empty());
}

TEST_CASE("no ASIO drivers means nothing to retarget", "[latency][asio][duplex]") {
    const std::vector<std::pair<std::uint32_t, std::string>> inputs{
        {1, "Speakers (Focusrite USB Audio)"},
    };
    CHECK(pluginrack::inputsToRetargetForClock(inputs, "Focusrite USB ASIO", {}).empty());
    CHECK(pluginrack::inputsToRetargetForClock(inputs, "", kAsio).empty());
}

TEST_CASE("the clock can follow a lone ASIO input onto its driver", "[latency][asio][duplex]") {
    // The user's rig: one input on KoordASIO, clock on a virtual Windows cable.
    // No duplex is possible across two devices, but the clock can move onto the
    // interface the input already owns, which removes the read delay entirely.
    const std::vector<std::string> inputs{"KoordASIO"};
    const std::vector<std::string> drivers{"KoordASIO"};
    CHECK(pluginrack::asioClockFromInputs(inputs, drivers) == "KoordASIO");
}

TEST_CASE("a WASAPI input name resolves to its ASIO driver", "[latency][asio][duplex]") {
    const std::vector<std::string> inputs{"Microphone (MOTU Audio Interface)"};
    CHECK(pluginrack::asioClockFromInputs(inputs, kAsio) == "MOTU Audio ASIO");
}

TEST_CASE("two different interfaces never make the clock guess",
          "[latency][asio][duplex]") {
    // Two inputs on two different drivers: no single clock choice is right.
    const std::vector<std::string> inputs{"Focusrite USB ASIO", "MOTU Audio ASIO"};
    CHECK(pluginrack::asioClockFromInputs(inputs, kAsio).empty());
    // A device that is not an ASIO driver adds no candidate.
    CHECK(pluginrack::asioClockFromInputs({"KoordASIO", "MOTU Audio ASIO"}, kAsio)
          == "MOTU Audio ASIO");
    CHECK(pluginrack::asioClockFromInputs({}, kAsio).empty());
    CHECK(pluginrack::asioClockFromInputs({"None - silent"}, kAsio).empty());
}

TEST_CASE("no ASIO driver means no clock move", "[latency][asio][duplex]") {
    CHECK(pluginrack::asioClockFromInputs({"KoordASIO"}, {}).empty());
}

