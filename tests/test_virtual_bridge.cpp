#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "pluginrack/VirtualAudioProtocol.h"
#include "pluginrack/VirtualBridge.h"
#include "pluginrack/VirtualRing.h"

namespace {

pluginrack::virt::EndpointDescriptor makeDesc(int slot, double rate) {
    pluginrack::virt::EndpointDescriptor desc{};
    desc.magic = pluginrack::virt::kMagic;
    desc.version = pluginrack::virt::kProtocolVersion;
    desc.slot = static_cast<std::uint32_t>(slot);
    desc.direction = static_cast<std::uint32_t>(
        pluginrack::virt::Direction::RenderToEngine);
    desc.sampleRate = static_cast<std::uint32_t>(rate);
    desc.channels = 2;
    return desc;
}

} // namespace

TEST_CASE("virtual capture open rejects bad protocol") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(64);
    pluginrack::virt::VirtualCapture capture(ring);
    auto desc = makeDesc(1, 48000.0);
    desc.version = 999;
    CHECK_FALSE(capture.open(desc, 48000.0, 64, 16));
    CHECK_FALSE(capture.isOpen());
    CHECK_FALSE(capture.error().empty());
}

TEST_CASE("virtual capture open rejects rate mismatch with warning") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(64);
    pluginrack::virt::VirtualCapture capture(ring);
    auto desc = makeDesc(1, 44100.0);
    CHECK_FALSE(capture.open(desc, 48000.0, 64, 16));
    CHECK_FALSE(capture.isOpen());
    CHECK(capture.error().find("44") != std::string::npos);
}

TEST_CASE("virtual capture feed then pull returns data in order") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(64);
    pluginrack::virt::VirtualCapture capture(ring);
    REQUIRE(capture.open(makeDesc(1, 48000.0), 48000.0, 64, 16));
    REQUIRE(capture.isOpen());

    float lIn[4] = {1, 2, 3, 4};
    float rIn[4] = {4, 3, 2, 1};
    REQUIRE(capture.feed(lIn, rIn, 4));

    float lOut[4] = {};
    float rOut[4] = {};
    CHECK(capture.pull(lOut, rOut, 4, 0.01) == 4);
    CHECK(lOut[0] == 1.0f);
    CHECK(lOut[3] == 4.0f);
    CHECK(rOut[0] == 4.0f);
}

TEST_CASE("virtual capture pull on empty is silent and counts underrun") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(64);
    pluginrack::virt::VirtualCapture capture(ring);
    REQUIRE(capture.open(makeDesc(1, 48000.0), 48000.0, 64, 16));
    float lOut[4] = {9, 9, 9, 9};
    float rOut[4] = {9, 9, 9, 9};
    CHECK(capture.pull(lOut, rOut, 4, 0.01) == 0);
    CHECK(lOut[0] == 0.0f);
    CHECK(capture.underruns() == 1);
}

TEST_CASE("virtual capture drift ratio rises when writer runs fast") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(256);
    pluginrack::virt::VirtualCapture capture(ring);
    REQUIRE(capture.open(makeDesc(1, 48000.0), 48000.0, 256, 64));
    // Writer pushes 2x per pull: fill grows, ratio must rise above 1.
    float lIn[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    float rIn[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    float lOut[8] = {};
    float rOut[8] = {};
    double lastRatio = 1.0;
    for (int i = 0; i < 40; ++i) {
        REQUIRE(capture.feed(lIn, rIn, 8));
        capture.pull(lOut, rOut, 4, 0.01);
        lastRatio = capture.ratio();
    }
    CHECK(lastRatio > 1.0);
}

TEST_CASE("virtual playback push then drain returns data") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(64);
    pluginrack::virt::VirtualPlayback playback(ring);
    pluginrack::virt::EndpointDescriptor desc = makeDesc(1, 48000.0);
    desc.direction = static_cast<std::uint32_t>(
        pluginrack::virt::Direction::EngineToCapture);
    REQUIRE(playback.open(desc, 48000.0, 64, 16));
    float lIn[4] = {5, 6, 7, 8};
    float rIn[4] = {8, 7, 6, 5};
    REQUIRE(playback.push(lIn, rIn, 4, 0.01));
    float lOut[4] = {};
    float rOut[4] = {};
    CHECK(playback.drain(lOut, rOut, 4) == 4);
    CHECK(lOut[0] == 5.0f);
    CHECK(rOut[3] == 5.0f);
}

TEST_CASE("virtual playback push on full counts overrun") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(8);
    pluginrack::virt::VirtualPlayback playback(ring);
    pluginrack::virt::EndpointDescriptor desc = makeDesc(1, 48000.0);
    desc.direction = static_cast<std::uint32_t>(
        pluginrack::virt::Direction::EngineToCapture);
    REQUIRE(playback.open(desc, 48000.0, 8, 4));
    float lIn[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    float rIn[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    REQUIRE(playback.push(lIn, rIn, 8, 0.01));
    CHECK_FALSE(playback.push(lIn, rIn, 1, 0.01));
    CHECK(playback.overruns() == 1);
}

TEST_CASE("virtual bridge disconnect makes capture silent until fed") {
    pluginrack::virt::VirtualRingBuffer ring;
    ring.prepare(64);
    pluginrack::virt::VirtualCapture capture(ring);
    REQUIRE(capture.open(makeDesc(1, 48000.0), 48000.0, 64, 16));
    float lIn[4] = {1, 2, 3, 4};
    float rIn[4] = {4, 3, 2, 1};
    REQUIRE(capture.feed(lIn, rIn, 4));
    ring.disconnect();
    float lOut[4] = {9, 9, 9, 9};
    float rOut[4] = {9, 9, 9, 9};
    CHECK(capture.pull(lOut, rOut, 4, 0.01) == 0);
    CHECK(lOut[0] == 0.0f);
    REQUIRE(capture.feed(lIn, rIn, 2));
    CHECK(capture.pull(lOut, rOut, 2, 0.01) == 2);
}
