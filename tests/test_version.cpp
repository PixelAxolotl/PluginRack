#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pluginrack/Version.h"

TEST_CASE("version string reports the library version") {
    REQUIRE(std::string(pluginrack::versionString()) == "0.1.0");
}
