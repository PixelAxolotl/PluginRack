#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "pluginrack/VirtualAudioProtocol.h"
#include "pluginrack/VirtualRing.h"

TEST_CASE("virtual protocol accepts its own magic and version") {
    pluginrack::virt::EndpointDescriptor desc{};
    desc.magic = pluginrack::virt::kMagic;
    desc.version = pluginrack::virt::kProtocolVersion;
    desc.slot = 0;
    desc.direction = static_cast<std::uint32_t>(
        pluginrack::virt::Direction::RenderToEngine);
    desc.sampleRate = 48000;
    desc.channels = 2;
    CHECK(pluginrack::virt::validate(desc));
}

TEST_CASE("virtual protocol rejects bad magic and future version") {
    pluginrack::virt::EndpointDescriptor desc{};
    desc.magic = pluginrack::virt::kMagic;
    desc.version = pluginrack::virt::kProtocolVersion;
    desc.slot = 0;
    desc.direction = static_cast<std::uint32_t>(
        pluginrack::virt::Direction::RenderToEngine);
    desc.sampleRate = 48000;
    desc.channels = 2;

    auto badMagic = desc;
    badMagic.magic = 0xdeadbeefu;
    CHECK_FALSE(pluginrack::virt::validate(badMagic));

    auto future = desc;
    future.version = pluginrack::virt::kProtocolVersion + 1;
    CHECK_FALSE(pluginrack::virt::validate(future));

    auto mono = desc;
    mono.channels = 1;
    CHECK_FALSE(pluginrack::virt::validate(mono));
}

TEST_CASE("virtual logical ids are stable and parse back to slots") {
    const std::string in1 = pluginrack::virt::makeVirtualInputId(1);
    const std::string out1 = pluginrack::virt::makeVirtualOutputId(1);
    CHECK(in1 == "pluginrack://vaio/in/1");
    CHECK(out1 == "pluginrack://vaio/out/1");
    CHECK(pluginrack::virt::isVirtualEndpointId(in1));
    CHECK(pluginrack::virt::isVirtualEndpointId(out1));
    CHECK_FALSE(pluginrack::virt::isVirtualEndpointId("ASIO::Realtek"));
    CHECK(pluginrack::virt::parseVirtualSlot(in1) == 1);
    CHECK(pluginrack::virt::parseVirtualSlot(out1) == 1);
    CHECK(pluginrack::virt::parseVirtualSlot("pluginrack://vaio/in/9") == 9);
    CHECK(pluginrack::virt::parseVirtualSlot("bogus") == -1);
}

TEST_CASE("virtual rate check stays inside the drift corrector range") {
    // DriftResampler defaults to +-0.5%: anything wider must disable.
    CHECK(pluginrack::virt::isRateCompatible(48000.0, 48000.0));
    CHECK(pluginrack::virt::isRateCompatible(48000.0, 48000.0 * 1.004));
    CHECK_FALSE(pluginrack::virt::isRateCompatible(48000.0, 48000.0 * 1.02));
    CHECK_FALSE(pluginrack::virt::isRateCompatible(48000.0, 44100.0));
    CHECK_FALSE(pluginrack::virt::isRateCompatible(48000.0, 0.0));
}

TEST_CASE("virtual ring writes and reads back in order across wrap") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(8);
    float lIn[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    float rIn[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    REQUIRE(ring.write(lIn, rIn, 5));
    REQUIRE(ring.available() == 5);

    float lOut[8] = {};
    float rOut[8] = {};
    REQUIRE(ring.read(lOut, rOut, 3) == 3);
    CHECK(lOut[0] == 1.0f);
    CHECK(lOut[2] == 3.0f);
    CHECK(rOut[0] == 8.0f);

    // Wrap: 2 left + 6 more = 8 used of 8 capacity.
    float lMore[6] = {9, 10, 11, 12, 13, 14};
    float rMore[6] = {1, 1, 1, 1, 1, 1};
    REQUIRE(ring.write(lMore, rMore, 6));
    REQUIRE(ring.available() == 8);
    REQUIRE(ring.read(lOut, rOut, 8) == 8);
    CHECK(lOut[0] == 4.0f);
    CHECK(lOut[1] == 5.0f);
    CHECK(lOut[2] == 9.0f);
    CHECK(lOut[7] == 14.0f);
    CHECK(ring.available() == 0);
}

TEST_CASE("virtual ring read on empty returns silence and counts underrun") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(8);
    float lOut[4] = {9, 9, 9, 9};
    float rOut[4] = {9, 9, 9, 9};
    CHECK(ring.read(lOut, rOut, 4) == 0);
    CHECK(lOut[0] == 0.0f);
    CHECK(rOut[3] == 0.0f);
    CHECK(ring.underruns() == 1);

    // Recovery: a later write reads back fine.
    float lIn[4] = {1, 2, 3, 4};
    float rIn[4] = {4, 3, 2, 1};
    REQUIRE(ring.write(lIn, rIn, 4));
    CHECK(ring.read(lOut, rOut, 4) == 4);
    CHECK(lOut[3] == 4.0f);
}

TEST_CASE("virtual ring write on full counts overrun and keeps newest") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(4);
    float lIn[4] = {1, 2, 3, 4};
    float rIn[4] = {1, 1, 1, 1};
    REQUIRE(ring.write(lIn, rIn, 4));
    CHECK_FALSE(ring.write(lIn, rIn, 1));
    CHECK(ring.overruns() == 1);

    float lOut[4] = {};
    float rOut[4] = {};
    REQUIRE(ring.read(lOut, rOut, 4) == 4);
    CHECK(lOut[0] == 1.0f);
    // After draining, writes work again.
    REQUIRE(ring.write(lIn, rIn, 2));
    CHECK(ring.available() == 2);
}

TEST_CASE("virtual ring allows one client and rejects a second") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(8);
    REQUIRE(ring.claimClient());
    CHECK_FALSE(ring.claimClient());
    CHECK(ring.clientConnected());
    ring.releaseClient();
    CHECK_FALSE(ring.clientConnected());
    REQUIRE(ring.claimClient());
    ring.releaseClient();
}

TEST_CASE("virtual ring disconnect makes reads silent until rewritten") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(8);
    float lIn[4] = {1, 2, 3, 4};
    float rIn[4] = {4, 3, 2, 1};
    REQUIRE(ring.write(lIn, rIn, 4));
    ring.disconnect();
    float lOut[4] = {9, 9, 9, 9};
    float rOut[4] = {9, 9, 9, 9};
    CHECK(ring.read(lOut, rOut, 4) == 0);
    CHECK(lOut[0] == 0.0f);
    // Reconnect by writing again.
    REQUIRE(ring.write(lIn, rIn, 2));
    CHECK(ring.read(lOut, rOut, 2) == 2);
    CHECK(lOut[1] == 2.0f);
}
