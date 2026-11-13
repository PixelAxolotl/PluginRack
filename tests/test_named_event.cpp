#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32

#include "NamedEvent.h"

namespace {

std::wstring uniqueName(const wchar_t* prefix) {
    static std::atomic<unsigned> counter{0};
    return std::wstring(prefix) + L"-" + std::to_wstring(::GetCurrentProcessId()) + L"-"
           + std::to_wstring(++counter);
}

} // namespace

TEST_CASE("a named event wakes a waiter and auto-resets") {
    const auto name = uniqueName(L"PluginRackTest-Event");
    pluginHostIpc::NamedEvent event;
    REQUIRE(event.create(name.c_str()));
    CHECK(event.isOpen());

    CHECK_FALSE(event.wait(0));
    REQUIRE(event.signal());
    CHECK(event.wait(1000));
    // Auto-reset: the signal was consumed by the successful wait.
    CHECK_FALSE(event.wait(0));

    event.close();
    CHECK_FALSE(event.isOpen());
}

TEST_CASE("a signal from the owning handle wakes a second handle") {
    const auto name = uniqueName(L"PluginRackTest-EventPair");
    pluginHostIpc::NamedEvent owner;
    REQUIRE(owner.create(name.c_str()));

    pluginHostIpc::NamedEvent waiter;
    REQUIRE(waiter.open(name.c_str()));

    std::atomic<bool> woke{false};
    std::thread waiting([&] { woke.store(waiter.wait(2000)); });
    owner.signal();
    waiting.join();
    CHECK(woke.load());

    // A pending signal can be discarded before it is consumed.
    REQUIRE(owner.signal());
    REQUIRE(owner.reset());
    CHECK_FALSE(waiter.wait(0));

    owner.close();
    waiter.close();
}

TEST_CASE("an opened handle can signal back to the owner") {
    const auto name = uniqueName(L"PluginRackTest-EventBack");
    pluginHostIpc::NamedEvent owner;
    REQUIRE(owner.create(name.c_str()));

    // The host side opens the response event and signals it; SYNCHRONIZE alone
    // would let the wait succeed but make SetEvent fail with access denied.
    pluginHostIpc::NamedEvent opened;
    REQUIRE(opened.open(name.c_str()));
    REQUIRE(opened.signal());
    CHECK(owner.wait(1000));

    owner.close();
    opened.close();
}

TEST_CASE("opening an event that was never created reports file not found") {
    const auto name = uniqueName(L"PluginRackTest-MissingEvent");
    pluginHostIpc::NamedEvent missing;
    CHECK_FALSE(missing.open(name.c_str()));
    CHECK(missing.error() == ERROR_FILE_NOT_FOUND);
}

#endif // _WIN32

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

// Same brace-depth extractor used by the underrun tests: the targeted source
// keeps braces out of string literals and comments.
std::string functionBody(const std::string& source, const std::string& signature) {
    const auto start = source.find(signature);
    if (start == std::string::npos) {
        FAIL("missing definition: " << signature);
    }
    const auto brace = source.find('{', start);
    if (brace == std::string::npos) {
        FAIL("missing body for: " << signature);
    }
    int depth = 0;
    for (auto i = brace; i < source.size(); ++i) {
        if (source[i] == '{') ++depth;
        if (source[i] == '}') {
            --depth;
            if (depth == 0) return source.substr(brace, i - brace + 1);
        }
    }
    FAIL("unbalanced braces in: " << signature);
    return {};
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("the plugin host audio loop is woken by events, not a timer tick",
          "[plugin][ipc][rt]") {
    const auto child = readSource("app/src/RestrictedHost.cpp");
    const auto app = readSource("app/src/PluginProcessHost.cpp");

    // The child waits on the audio and control events together and acks on the
    // response event. WaitForMultipleObjects keeps the control channel off the
    // system timer tick as well, which a polled control request would not.
    const auto host = functionBody(child, "int RestrictedHost::runHost");
    CHECK(contains(host, "WaitForMultipleObjects"));
    CHECK(contains(host, "requestEvent.nativeHandle()"));
    CHECK(contains(host, "responseEvent.signal()"));
    // A 1 ms sleep is a timer-tick wait. Windows 11 ignores the process's
    // timer-resolution request in the background, which is exactly what made
    // the audio stutter when the window was covered.
    CHECK_FALSE(contains(host, "juce::Thread::sleep(1)"));

    // The app signals the request after publishing and waits on the response
    // instead of spinning on the audio thread.
    const auto submit = functionBody(app, "bool PluginProcessHost::submitBlock");
    CHECK(contains(submit, "requestEvent.signal()"));
    const auto fetch = functionBody(app, "bool PluginProcessHost::fetchBlock");
    CHECK(contains(fetch, "responseEvent.wait("));
    CHECK_FALSE(contains(fetch, "juce::Thread::yield()"));

    // Both sides must create/open the same channels before a host is usable.
    const auto open = functionBody(app, "bool PluginProcessHost::openChannel");
    CHECK(contains(open, "requestEvent.create("));
    CHECK(contains(open, "responseEvent.create("));
    CHECK(contains(host, "requestEvent.open("));
    CHECK(contains(host, "responseEvent.open("));

    const auto names = readSource("app/src/PluginHostIpc.h");
    CHECK(contains(names, "requestEventNameFor("));
    CHECK(contains(names, "responseEventNameFor("));
    CHECK(contains(names, "controlRequestEventNameFor("));
    CHECK(contains(names, "controlResponseEventNameFor("));
}
