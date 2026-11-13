#include <catch2/catch_test_macros.hpp>

#include "CrashContext.h"

#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string readSource(const char* relativePath) {
    const std::string path = std::string(PLUGINRACK_SOURCE_DIR) + "/" + relativePath;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        FAIL("cannot open " << path);
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

int countOf(const std::string& haystack, const std::string& needle) {
    int count = 0;
    for (auto pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("crash label for the same insert text is stable and stored once") {
    const auto before = crash_context::labelCount();
    const char* first = crash_context::registerLabel("Vox insert 3 \"Scope.vst3\"");
    const char* second = crash_context::registerLabel("Vox insert 3 \"Scope.vst3\"");
    CHECK(first == second); // the audio thread keeps a raw pointer, so no realloc
    CHECK(crash_context::labelCount() == before + 1);

    const char* other = crash_context::registerLabel("Vox insert 4 \"Scope.vst3\"");
    CHECK(other != first);
    CHECK(std::string(other) == "Vox insert 4 \"Scope.vst3\"");
}

TEST_CASE("current render label round-trips and clears") {
    CHECK(crash_context::currentRender().empty());

    const char* label = crash_context::registerLabel("Bus insert 1 \"Scope.vst3\"");
    crash_context::setCurrentRender(label);
    CHECK(crash_context::currentRender() == "Bus insert 1 \"Scope.vst3\"");

    crash_context::clearCurrentRender();
    CHECK(crash_context::currentRender().empty());
}

TEST_CASE("the render label is per thread") {
    crash_context::clearCurrentRender();
    const char* label = crash_context::registerLabel("Vox insert 9 \"Scope.vst3\"");

    // Another thread must not see the audio thread's insert: a crash there
    // would otherwise report a plugin that was rendered somewhere else.
    bool workerSawEmpty = false;
    bool workerRoundTripped = false;
    std::thread worker([label, &workerSawEmpty, &workerRoundTripped] {
        workerSawEmpty = crash_context::currentRender().empty();
        crash_context::setCurrentRender(label);
        workerRoundTripped = crash_context::currentRender() == "Vox insert 9 \"Scope.vst3\"";
    });
    worker.join();

    CHECK(workerSawEmpty);
    CHECK(workerRoundTripped);
    CHECK(crash_context::currentRender().empty());
}

TEST_CASE("the audio path publishes a label around every plugin render",
          "[plugin][crash][rt]") {
    const auto engine = readSource("app/src/AudioEngine.cpp");
    const auto main = readSource("app/src/Main.cpp");

    const int renders = countOf(engine, "pluginHost.processBlock(");
    CHECK(renders >= 2); // channel inserts and output inserts
    // One set/clear pair per render site. Missing one leaves that strip's
    // crashes unattributed, which is exactly what this is for.
    CHECK(countOf(engine, "crash_context::setCurrentRender(") == renders);
    CHECK(countOf(engine, "crash_context::clearCurrentRender()") == renders);

    // Every insert creation path assigns a label, or the pointer stays null and
    // the report says nothing useful.
    CHECK(countOf(engine, "makeCrashLabel(") >= 4);

    CHECK(main.find("crash_context::currentRender()") != std::string::npos);
    CHECK(main.find("\"rendering: \"") != std::string::npos);
}
