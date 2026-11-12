#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <pluginrack/DeviceCache.h>

namespace {

using pluginrack::DeviceCache;
using pluginrack::EndpointCapabilities;

EndpointCapabilities usbCapabilities() {
    EndpointCapabilities capabilities;
    capabilities.inputChannels = 2;
    capabilities.outputChannels = 2;
    capabilities.sampleRates = {44100, 48000, 96000};
    capabilities.negotiatedRate = 48000;
    capabilities.negotiatedBuffer = 480;
    capabilities.mode = "shared";
    capabilities.probeMs = 16204;
    capabilities.lastGood = "2026-02-11T09:41:02Z";
    capabilities.lastGoodUnixMs = 1'770'000'000'000LL;
    return capabilities;
}

} // namespace

TEST_CASE("device capabilities round-trip through JSON", "[device][cache]") {
    DeviceCache cache;
    cache.put("USB Audio", usbCapabilities());
    cache.put("Built-in Output", EndpointCapabilities{});

    const std::string json = cache.toJson();
    std::vector<std::string> errors;
    const auto loaded = DeviceCache::fromJson(json, &errors);
    REQUIRE(loaded.has_value());
    CHECK(errors.empty());
    CHECK(loaded->size() == 2);

    const EndpointCapabilities* usb = loaded->find("USB Audio");
    REQUIRE(usb != nullptr);
    CHECK(usb->inputChannels == 2);
    CHECK(usb->outputChannels == 2);
    CHECK(usb->sampleRates == std::vector<int>{44100, 48000, 96000});
    CHECK(usb->negotiatedRate == 48000);
    CHECK(usb->negotiatedBuffer == 480);
    CHECK(usb->mode == "shared");
    CHECK(usb->probeMs == 16204);
    CHECK(usb->lastGood == "2026-02-11T09:41:02Z");
    CHECK(usb->lastGoodUnixMs == 1'770'000'000'000LL);
}

TEST_CASE("an unknown schema is rejected without throwing", "[device][cache]") {
    std::vector<std::string> errors;
    const auto future = DeviceCache::fromJson(
        R"({"schema": 999, "endpoints": {}})", &errors);
    CHECK_FALSE(future.has_value());
    REQUIRE_FALSE(errors.empty());

    errors.clear();
    const auto malformed = DeviceCache::fromJson("{not json", &errors);
    CHECK_FALSE(malformed.has_value());
    REQUIRE_FALSE(errors.empty());

    errors.clear();
    const auto old = DeviceCache::fromJson(
        R"({"schema": 0, "endpoints": {}})", &errors);
    CHECK_FALSE(old.has_value());
}

TEST_CASE("a stale cache falls through to the live probe", "[device][cache]") {
    DeviceCache cache;
    EndpointCapabilities cached = usbCapabilities();
    cached.lastGoodUnixMs = 1'000'000;
    cache.put("USB Audio", cached);

    const std::int64_t now = 1'000'000 + DeviceCache::kDefaultMaxAgeMs + 1;
    CHECK(DeviceCache::isStale(cached, now));
    CHECK_FALSE(cache.lookupUsable("USB Audio", now).has_value());

    // A failed probe with a stale cache still forces the caller to probe.
    CHECK_FALSE(pluginrack::resolveCapabilities(cache, "USB Audio", false,
                                                EndpointCapabilities{}, now)
                    .has_value());

    // A fresh entry is advisory truth when the probe has not run.
    const std::int64_t freshNow = cached.lastGoodUnixMs + 1000;
    REQUIRE(cache.lookupUsable("USB Audio", freshNow).has_value());

    // A successful probe always wins.
    EndpointCapabilities live;
    live.negotiatedRate = 96000;
    const auto resolved = pluginrack::resolveCapabilities(
        cache, "USB Audio", true, live, now);
    REQUIRE(resolved.has_value());
    CHECK(resolved->negotiatedRate == 96000);
}
