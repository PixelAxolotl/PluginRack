#include <catch2/catch_test_macros.hpp>

#include <pluginrack/AsioDeviceMatch.h>

namespace {

const std::vector<std::string> kInterfaces{
    "Focusrite USB ASIO",
    "MOTU Audio ASIO",
    "ASIO4ALL v2",
};

} // namespace

TEST_CASE("a WASAPI name finds its ASIO counterpart", "[latency][asio]") {
    CHECK(pluginrack::bestAsioDeviceMatch("Speakers (Focusrite USB Audio)", kInterfaces)
          == "Focusrite USB ASIO");
    CHECK(pluginrack::bestAsioDeviceMatch("Focusrite USB Audio", kInterfaces)
          == "Focusrite USB ASIO");
    CHECK(pluginrack::bestAsioDeviceMatch("Microphone (MOTU Audio Interface)", kInterfaces)
          == "MOTU Audio ASIO");
}

TEST_CASE("generic words alone never match", "[latency][asio]") {
    // Both names share "audio"/"usb" and nothing else meaningful.
    CHECK(pluginrack::bestAsioDeviceMatch("Speakers (Generic USB Audio)", kInterfaces).empty());
    CHECK(pluginrack::bestAsioDeviceMatch("", kInterfaces).empty());
    CHECK(pluginrack::bestAsioDeviceMatch("Speakers", {}).empty());
}

TEST_CASE("the strongest shared word wins", "[latency][asio]") {
    const std::vector<std::string> many{"Focusrite USB ASIO", "MOTU Audio ASIO"};
    // "motu" is the distinctive token here, not the generic ones.
    CHECK(pluginrack::bestAsioDeviceMatch("Speakers (MOTU Audio)", many) == "MOTU Audio ASIO");
    CHECK(pluginrack::bestAsioDeviceMatch("Speakers (Focusrite USB)", many)
          == "Focusrite USB ASIO");
}

TEST_CASE("an exact ASIO name matches itself", "[latency][asio]") {
    CHECK(pluginrack::bestAsioDeviceMatch("Focusrite USB ASIO", kInterfaces)
          == "Focusrite USB ASIO");
    CHECK(pluginrack::bestAsioDeviceMatch("asio4all v2", kInterfaces) == "ASIO4ALL v2");
}
