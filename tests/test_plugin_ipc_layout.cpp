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

TEST_CASE("every param ring drop consumes its own sequence number") {
    // Regression: the drop branch used to store `head + 1`, and the head does
    // not move while the ring is full, so back-to-back drops re-published the
    // last delivered sequence and a later drain reported "nothing lost".
    ParamRing ring;
    CHECK(ring.paramGapCount.load() == 0);

    const auto fill = [&ring] {
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(kParamRingSlots); ++i) {
            ParamMsg msg;
            msg.id = i;
            REQUIRE(paramRingPush(ring, msg));
        }
    };
    const auto drain = [&ring] {
        std::uint64_t lastSeq = 0;
        ParamMsg msg;
        while (paramRingPop(ring, msg)) {
            lastSeq = msg.seq;
        }
        return lastSeq;
    };

    // Cycle one: fill, two back-to-back drops while full, drain. Each drop
    // must count as exactly one lost message.
    fill();
    ParamMsg overflow;
    overflow.id = 9999;
    CHECK_FALSE(paramRingPush(ring, overflow));
    CHECK_FALSE(paramRingPush(ring, overflow));
    const auto firstDelivered = drain();
    CHECK(firstDelivered == static_cast<std::uint64_t>(kParamRingSlots));
    CHECK(paramRingLostMessages(ring) == 2);

    // The next delivered message skips both consumed-but-undelivered numbers,
    // so a draining receiver sees the gap with paramSeqGapped.
    ParamMsg resumed;
    resumed.id = 9000;
    REQUIRE(paramRingPush(ring, resumed));
    ParamMsg popped;
    REQUIRE(paramRingPop(ring, popped));
    CHECK(popped.seq == firstDelivered + 3);
    CHECK(paramSeqGapped(firstDelivered, popped.seq));

    // Cycle two: a drop after the counters moved on must still advance. With
    // the old head + 1 store the counter fell back below the delivered
    // sequences and the loss read back as zero.
    fill();
    CHECK_FALSE(paramRingPush(ring, overflow));
    CHECK_FALSE(paramRingPush(ring, overflow));
    const auto secondDelivered = drain();
    CHECK(secondDelivered == static_cast<std::uint64_t>(kParamRingSlots) * 2 + 3);
    CHECK(paramRingLostMessages(ring) == 2);

    // And a clean message after the second cycle closes the gap again.
    REQUIRE(paramRingPush(ring, resumed));
    REQUIRE(paramRingPop(ring, popped));
    CHECK(popped.seq == secondDelivered + 3);
    CHECK(paramSeqGapped(secondDelivered, popped.seq));
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

TEST_CASE("param ring drain up to a sequence stops at the cutoff") {
    ParamRing ring;
    for (std::uint32_t i = 1; i <= 5; ++i) {
        ParamMsg msg;
        msg.id = i;
        REQUIRE(paramRingPush(ring, msg));
    }

    // The cutoff is inclusive, and a cutoff below every queued sequence drains
    // nothing.
    CHECK(paramRingDrainUpTo(ring, 0) == 0);
    CHECK(paramRingDrainUpTo(ring, 3) == 3);
    CHECK(ring.consumerSeq.load() == 3);

    // The entries published after the cutoff stay queued, in order and with no
    // gap: the state that carried sequence 3 does not cover them.
    ParamMsg next;
    REQUIRE(paramRingPop(ring, next));
    CHECK(next.id == 4);
    CHECK(next.seq == 4);
    CHECK_FALSE(paramSeqGapped(3, next.seq));
    REQUIRE(paramRingPop(ring, next));
    CHECK(next.id == 5);
    CHECK(next.seq == 5);
    CHECK_FALSE(paramRingPop(ring, next));
}

TEST_CASE("param ring drain up to a sequence keeps a dropped edit detectable") {
    ParamRing ring;
    for (std::uint32_t i = 1; i <= 3; ++i) {
        ParamMsg msg;
        msg.id = i;
        REQUIRE(paramRingPush(ring, msg));
    }
    // Emulate the full-ring drop at sequence 4: a real gap that early needs 512
    // messages already queued, but the effect on the counters is this one
    // store - the producer advances without anything entering the ring.
    ring.producerSeq.store(ring.producerSeq.load(std::memory_order_acquire) + 1,
                           std::memory_order_release);
    ParamMsg later;
    later.id = 5;
    REQUIRE(paramRingPush(ring, later));
    REQUIRE(ring.producerSeq.load() == 5);

    CHECK(paramRingDrainUpTo(ring, 3) == 3);

    // The delivered message skipped the dropped one, so the child still sees
    // the gap even though the state push swallowed sequences 1..3.
    ParamMsg next;
    REQUIRE(paramRingPop(ring, next));
    CHECK(next.id == 5);
    CHECK(next.seq == 5);
    CHECK(paramSeqGapped(3, next.seq));
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
