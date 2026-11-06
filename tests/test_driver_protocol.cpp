#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

#include "pluginrack/VirtualAudioProtocol.h"
#include "pluginrack/VirtualRing.h"
#include "../../driver/PluginRackDriverProtocol.h"

// The kernel header and the user-mode header must agree: same magic,
// same version, same drift range, same stereo-only rule. A bump here
// without a bump there breaks connect-time validation.
TEST_CASE("driver protocol matches user-mode contract") {
    CHECK(pluginrack::driver::kMagic == pluginrack::virt::kMagic);
    CHECK(pluginrack::driver::kProtocolVersion ==
          pluginrack::virt::kProtocolVersion);
    CHECK(pluginrack::driver::kMaxDriftRatio == 0.005);

    pluginrack::driver::SharedRingHeader h{};
    h.magic = pluginrack::driver::kMagic;
    h.version = pluginrack::driver::kProtocolVersion;
    h.slot = 0;
    h.direction = static_cast<std::uint32_t>(
        pluginrack::driver::Direction::RenderToEngine);
    h.sampleRate = 48000;
    h.channels = 2;
    h.capacityFrames = 1024;
    CHECK(pluginrack::driver::validateHeader(h));

    h.version += 1;
    CHECK_FALSE(pluginrack::driver::validateHeader(h));
    h.version = pluginrack::driver::kProtocolVersion;

    h.channels = 1;
    CHECK_FALSE(pluginrack::driver::validateHeader(h));
    h.channels = 2;

    h.capacityFrames = pluginrack::driver::kMaxRingFrames + 1;
    CHECK_FALSE(pluginrack::driver::validateHeader(h));
}

TEST_CASE("driver header is a fixed-size C ABI") {
    // No vtable, no bool, no pointer: identical layout in kernel and user.
    static_assert(sizeof(pluginrack::driver::SharedRingHeader) % 4 == 0);
    CHECK(sizeof(std::uint32_t) == 4);
    CHECK(offsetof(pluginrack::driver::SharedRingHeader, magic) == 0);
    CHECK(offsetof(pluginrack::driver::SharedRingHeader, version) == 4);
    CHECK(offsetof(pluginrack::driver::SharedRingHeader, slot) == 8);
    CHECK(offsetof(pluginrack::driver::SharedRingHeader, direction) == 12);
    CHECK(offsetof(pluginrack::driver::SharedRingHeader, sampleRate) == 16);
    CHECK(offsetof(pluginrack::driver::SharedRingHeader, channels) == 20);
    CHECK(offsetof(pluginrack::driver::SharedRingHeader, capacityFrames) == 24);
    CHECK(offsetof(pluginrack::driver::SharedRingHeader, state) == 28);
    // head/tail are 64-byte aligned so the two sides never share a line.
    CHECK(offsetof(pluginrack::driver::SharedRingHeader, headFrames) % 64 == 0);
    CHECK(offsetof(pluginrack::driver::SharedRingHeader, tailFrames) % 64 == 0);
}

TEST_CASE("driver clamp keeps kernel copies inside the ring") {
    using pluginrack::driver::clampAvailable;
    CHECK(clampAvailable(5, 2, 8) == 3);
    CHECK(clampAvailable(2, 5, 8) == 5); // wrapped
    CHECK(clampAvailable(3, 3, 8) == 0);
    CHECK(clampAvailable(99, 4, 0) == 0);
    // Unclamped offsets from a stale client cannot escape the ring.
    CHECK(clampAvailable(1000000, 3, 1024) < 1024);
}

TEST_CASE("driver v1 topology fits the protocol table") {
    CHECK(pluginrack::driver::kRenderEndpointsV1 == 2);
    CHECK(pluginrack::driver::kCaptureEndpointsV1 == 2);
    CHECK(pluginrack::driver::kRenderEndpointsV1 +
              pluginrack::driver::kCaptureEndpointsV1 <=
          2 * pluginrack::driver::kMaxEndpointPairs);
    // Logical ids cover every v1 slot.
    for (std::uint32_t s = 1; s <= pluginrack::driver::kRenderEndpointsV1;
         ++s) {
        CHECK(pluginrack::virt::parseVirtualSlot(
                  pluginrack::virt::makeVirtualInputId(static_cast<int>(s))) ==
              static_cast<int>(s));
    }
}
