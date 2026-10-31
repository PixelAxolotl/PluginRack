#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "pluginrack/CaptureCursor.h"

namespace {

constexpr int kFrames = 256;
constexpr int kCapacity = 2048;
constexpr int kTarget = 512;

} // namespace

TEST_CASE("steady writer yields contiguous full blocks") {
    pluginrack::CaptureReadCursor cursor;
    cursor.prepare(kCapacity, kTarget);

    std::uint64_t written = static_cast<std::uint64_t>(kTarget);
    std::int64_t previousStart = -1;
    for (std::uint64_t epoch = 1; epoch < 200; ++epoch) {
        const auto window = cursor.nextBlock(written, kFrames, epoch);
        REQUIRE_FALSE(window.underrun);
        REQUIRE_FALSE(window.overrun);
        REQUIRE(window.available == kFrames);
        REQUIRE(window.ratio == 1.0);
        if (previousStart >= 0) {
            REQUIRE(window.start == previousStart + kFrames);
        }
        previousStart = window.start;
        written += static_cast<std::uint64_t>(kFrames);
    }
}

TEST_CASE("callback jitter never repeats or skips a whole block") {
    pluginrack::CaptureReadCursor cursor;
    cursor.prepare(kCapacity, kTarget);

    std::uint64_t written = static_cast<std::uint64_t>(kTarget);
    std::int64_t previousStart = -1;
    int underruns = 0;
    int overruns = 0;
    for (std::uint64_t epoch = 1; epoch < 200; ++epoch) {
        const auto window = cursor.nextBlock(written, kFrames, epoch);
        underruns += window.underrun ? 1 : 0;
        overruns += window.overrun ? 1 : 0;
        if (previousStart >= 0) {
            const auto start = static_cast<double>(window.start) + window.fraction;
            const auto previousEnd =
                static_cast<double>(previousStart) + kFrames;
            // A few samples of overlap/gap from the rate correction are fine;
            // repeating or skipping an entire block is not.
            REQUIRE(start >= previousEnd - 4.0);
            REQUIRE(start <= previousEnd + 4.0);
        }
        previousStart = window.start;
        // Input callbacks arrive 128 or 384 frames apart, not 256.
        written += ((epoch % 2) == 0) ? 128u : 384u;
    }
    REQUIRE(underruns == 0);
    REQUIRE(overruns == 0);
}

TEST_CASE("slow input clock stays locked to the target delay") {
    pluginrack::CaptureReadCursor cursor;
    cursor.prepare(kCapacity, kTarget);
    REQUIRE(cursor.targetDelay() == kTarget);

    std::uint64_t written = static_cast<std::uint64_t>(kTarget);
    std::uint64_t epoch = 1;
    // Prime, then run 2000 blocks where the input produces 255 frames per
    // output block (about 390 ppm slow).
    for (int block = 0; block < 2000; ++block) {
        const auto window = cursor.nextBlock(written, kFrames, epoch++);
        if (block > 1) {
            REQUIRE_FALSE(window.underrun);
            REQUIRE_FALSE(window.overrun);
        }
        written += 255;
    }

    const auto last = cursor.nextBlock(written, kFrames, epoch);
    REQUIRE_FALSE(last.underrun);
    REQUIRE_FALSE(last.overrun);
    const auto delay = static_cast<double>(written) - last.start - last.fraction;
    REQUIRE(delay > kTarget - 8.0);
    REQUIRE(delay < kTarget + 8.0);
}

TEST_CASE("repeated pulls in one epoch return the same window") {
    pluginrack::CaptureReadCursor cursor;
    cursor.prepare(kCapacity, kTarget);
    std::uint64_t written = static_cast<std::uint64_t>(kTarget);

    const auto first = cursor.nextBlock(written, kFrames, 1);
    const auto second = cursor.nextBlock(written, kFrames, 1);
    REQUIRE(first.start == second.start);
    REQUIRE(first.fraction == second.fraction);
    REQUIRE(first.ratio == second.ratio);

    const auto next = cursor.nextBlock(written, kFrames, 2);
    REQUIRE(next.start > first.start);
}

TEST_CASE("writer lapping the ring is reported and resynced") {
    pluginrack::CaptureReadCursor cursor;
    cursor.prepare(kCapacity, kTarget);
    std::uint64_t written = static_cast<std::uint64_t>(kTarget);
    REQUIRE_FALSE(cursor.nextBlock(written, kFrames, 1).overrun);
    written += static_cast<std::uint64_t>(kCapacity * 4);
    const auto window = cursor.nextBlock(written, kFrames, 3);
    REQUIRE(window.overrun);
    REQUIRE(window.available == kFrames);
    // Resynced to the configured delay instead of reading overwritten data.
    const auto delay = static_cast<double>(written) - window.start - window.fraction;
    REQUIRE(delay <= static_cast<double>(kTarget));
    REQUIRE(delay > static_cast<double>(kTarget) - 2.0);
}

TEST_CASE("input stall reports an underrun and recovers") {
    pluginrack::CaptureReadCursor cursor;
    cursor.prepare(kCapacity, kTarget);
    std::uint64_t written = static_cast<std::uint64_t>(kTarget);
    REQUIRE_FALSE(cursor.nextBlock(written, kFrames, 1).underrun);
    // The second pull drains the remaining delay; still a full block.
    REQUIRE_FALSE(cursor.nextBlock(written, kFrames, 2).underrun);

    // Writer stayed stalled: less than a full block is available now.
    const auto starved = cursor.nextBlock(written, kFrames, 3);
    REQUIRE(starved.underrun);
    REQUIRE(starved.available < kFrames);

    // Writer resumes; the cursor continues without replaying a stale block.
    written += static_cast<std::uint64_t>(kFrames * 4);
    const auto recovered = cursor.nextBlock(written, kFrames, 4);
    REQUIRE_FALSE(recovered.underrun);
    REQUIRE_FALSE(recovered.overrun);
    REQUIRE(recovered.start >= starved.start);
}
