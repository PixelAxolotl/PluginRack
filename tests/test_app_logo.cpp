#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::string& relativePath) {
    const std::string path = std::string(PLUGINRACK_SOURCE_DIR) + "/" + relativePath;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        FAIL("cannot open " << path);
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("app logo is wired into exe icon and toolbar", "[app-logo]") {
    const auto cmake = readFile("app/CMakeLists.txt");
    CHECK(contains(cmake, "ICON_BIG"));
    CHECK(contains(cmake, "ICON_SMALL"));
    CHECK(contains(cmake, "logo_dark"));

    const auto toolbar = readFile("app/src/MainComponent.cpp");
    CHECK(contains(toolbar, "logoImage"));
    CHECK(contains(toolbar, "logo_dark"));

    const auto main = readFile("app/src/Main.cpp");
    CHECK(contains(main, "logo_dark"));
}
