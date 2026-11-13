#include <catch2/catch_test_macros.hpp>

#include "PluginIpcLayout.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace pluginHostIpc;

TEST_CASE("param ring round-trips a message") {
    ParamRing ring;
    ParamMsg sent;
    sent.id = 42;
    sent.value = 0.25f;
    sent.flags = kParamFlagGestureActive;

    REQUIRE(paramRingPush(ring, sent));

    ParamMsg received;
    REQUIRE(paramRingPop(ring, received));
    CHECK(received.id == 42);
    CHECK(received.value == 0.25f);
    CHECK(received.flags == kParamFlagGestureActive);
    CHECK(received.seq == 1);

    ParamMsg empty;
    CHECK_FALSE(paramRingPop(ring, empty));
}

TEST_CASE("param ring preserves order across a wrap") {
    ParamRing ring;
    // Fill once, drain once: head and tail now both point past the first lap,
    // so the next pushes wrap around the end of the slot array.
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(kParamRingSlots); ++i) {
        ParamMsg msg;
        msg.id = i;
        msg.value = static_cast<float>(i);
        REQUIRE(paramRingPush(ring, msg));
    }
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(kParamRingSlots); ++i) {
        ParamMsg msg;
        REQUIRE(paramRingPop(ring, msg));
        REQUIRE(msg.id == i);
    }
    ParamMsg leftover;
    CHECK_FALSE(paramRingPop(ring, leftover));

    for (std::uint32_t i = 0; i < 8; ++i) {
        ParamMsg msg;
        msg.id = 1000 + i;
        REQUIRE(paramRingPush(ring, msg));
    }
    for (std::uint32_t i = 0; i < 8; ++i) {
        ParamMsg msg;
        REQUIRE(paramRingPop(ring, msg));
        CHECK(msg.id == 1000 + i);
    }
}

TEST_CASE("a full param ring drops the message and leaves a detectable gap") {
    ParamRing ring;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(kParamRingSlots); ++i) {
        ParamMsg msg;
        msg.id = i;
        REQUIRE(paramRingPush(ring, msg));
    }

    ParamMsg overflow;
    overflow.id = 9999;
    CHECK_FALSE(paramRingPush(ring, overflow));

    // The dropped message consumed a sequence number but never entered the
    // ring, so the consumer must learn about it from the counters after it has
    // drained - not from a gap between two delivered messages.
    ParamMsg drained;
    while (paramRingPop(ring, drained)) {
    }
    CHECK(paramRingLostMessages(ring) == 1);

    // Draining makes room again, and a clean drain reports no loss.
    ParamMsg after;
    after.id = 7;
    REQUIRE(paramRingPush(ring, after));
    REQUIRE(paramRingPop(ring, drained));
    CHECK(paramRingLostMessages(ring) == 0);
}

TEST_CASE("consecutive param messages are not reported as a gap") {
    ParamRing ring;
    ParamMsg a;
    a.id = 1;
    ParamMsg b;
    b.id = 2;
    REQUIRE(paramRingPush(ring, a));
    REQUIRE(paramRingPush(ring, b));

    ParamMsg first;
    ParamMsg second;
    REQUIRE(paramRingPop(ring, first));
    REQUIRE(paramRingPop(ring, second));
    CHECK_FALSE(paramSeqGapped(first.seq, second.seq));
}

TEST_CASE("control payload copies text and stays terminated") {
    // The control section is ~1 MiB; a stack instance overflows the default
    // 1 MB thread stack, so tests own it on the heap like the shared mapping.
    auto section = std::make_unique<ControlSection>();
    const char* json = "{\"ping\":{}}";

    REQUIRE(controlSetPayload(*section, json, std::char_traits<char>::length(json)));
    CHECK(section->payloadBytes.load() == std::char_traits<char>::length(json));
    CHECK(controlPayloadTerminated(*section));
    CHECK(std::string(section->payload) == std::string(json));
}

TEST_CASE("control payload refuses text that does not fit instead of truncating") {
    auto section = std::make_unique<ControlSection>();
    std::vector<char> big(kControlPayloadBytes + 1, 'x');

    CHECK_FALSE(controlSetPayload(*section, big.data(), big.size()));
    CHECK(section->payloadBytes.load() == 0);

    // One byte under the cap still fits, and still terminates.
    std::vector<char> fits(kControlPayloadBytes - 1, 'y');
    REQUIRE(controlSetPayload(*section, fits.data(), fits.size()));
    CHECK(controlPayloadTerminated(*section));
}

TEST_CASE("control begin/end write brackets the payload with an even sequence") {
    auto section = std::make_unique<ControlSection>();
    controlBeginWrite(*section);
    CHECK(section->blockSequence.load() % 2 == 1);
    controlEndWrite(*section);
    CHECK(section->blockSequence.load() % 2 == 0);
    CHECK(section->blockSequence.load() == 2);
}

TEST_CASE("control op names match the wire vocabulary") {
    CHECK(std::string(controlOpName(ControlOp::prepare)) == "prepare");
    CHECK(std::string(controlOpName(ControlOp::setState)) == "setState");
    CHECK(std::string(controlOpName(ControlOp::getState)) == "getState");
    CHECK(std::string(controlOpName(ControlOp::getLatency)) == "getLatency");
    CHECK(std::string(controlOpName(ControlOp::ping)) == "ping");
    CHECK(std::string(controlOpName(ControlOp::none)) == "none");
}
