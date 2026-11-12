#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

#include "NamedSection.h"

#include <atomic>
#include <string>

namespace {

std::wstring uniqueName(const wchar_t* prefix) {
    static std::atomic<unsigned> counter{0};
    return std::wstring(prefix) + L"-" + std::to_wstring(::GetCurrentProcessId()) + L"-"
           + std::to_wstring(++counter);
}

} // namespace

TEST_CASE("named section stays openable while its owner is alive") {
    const auto name = uniqueName(L"PluginRackTest-Section");
    constexpr std::size_t bytes = 4096;

    pluginHostIpc::NamedSection owner;
    REQUIRE(owner.create(name.c_str(), bytes));
    REQUIRE(owner.data() != nullptr);

    pluginHostIpc::NamedSection host;
    REQUIRE(host.open(name.c_str(), bytes));
    REQUIRE(host.data() != nullptr);

    auto* ownerBytes = static_cast<unsigned char*>(owner.data());
    auto* hostBytes = static_cast<unsigned char*>(host.data());
    ownerBytes[0] = 0x2A;
    ownerBytes[bytes - 1] = 0x7F;
    CHECK(hostBytes[0] == 0x2A);
    CHECK(hostBytes[bytes - 1] == 0x7F);

    owner.close();
    host.close();
}

TEST_CASE("opening a name that was never created reports file not found") {
    const auto name = uniqueName(L"PluginRackTest-Missing");
    pluginHostIpc::NamedSection section;
    CHECK_FALSE(section.open(name.c_str(), 4096));
    CHECK(section.error() == ERROR_FILE_NOT_FOUND);
}

TEST_CASE("closing the owning section releases the name") {
    const auto name = uniqueName(L"PluginRackTest-Released");
    pluginHostIpc::NamedSection owner;
    REQUIRE(owner.create(name.c_str(), 4096));
    owner.close();

    pluginHostIpc::NamedSection later;
    CHECK_FALSE(later.open(name.c_str(), 4096));
    CHECK(later.error() == ERROR_FILE_NOT_FOUND);
}

#endif // _WIN32
