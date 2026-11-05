#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <pluginrack/AudioTelemetry.h>

namespace {

using pluginrack::AudioTelemetry;

AudioTelemetry::Sample sampleWith(std::uint32_t renderNanos, std::int32_t fill = 256,
                                  std::uint16_t flags = 0) {
    AudioTelemetry::Sample sample;
    sample.blockFrames = 128;
    sample.renderNanos = renderNanos;
    sample.outputFillFrames = fill;
    sample.flags = flags;
    return sample;
}

} // namespace

TEST_CASE("an empty telemetry summarises to zero", "[latency][telemetry]") {
    AudioTelemetry telemetry;
    const auto summary = telemetry.summarise(128, 48000.0);
    CHECK(summary.count == 0);
    CHECK(summary.renderP99Ms == 0.0);
    CHECK(summary.renderWorstOverBlockPercent == 0.0);
}

TEST_CASE("a single sample reports itself at every percentile", "[latency][telemetry]") {
    AudioTelemetry telemetry;
    telemetry.push(sampleWith(1000000));  // 1 ms
    const auto summary = telemetry.summarise(128, 48000.0);
    REQUIRE(summary.count == 1);
    CHECK(summary.renderP50Ms == Catch::Approx(1.0));
    CHECK(summary.renderP95Ms == Catch::Approx(1.0));
    CHECK(summary.renderP99Ms == Catch::Approx(1.0));
    CHECK(summary.renderMaxMs == Catch::Approx(1.0));
}

TEST_CASE("percentiles follow the sorted render times", "[latency][telemetry]") {
    AudioTelemetry telemetry;
    // 1..10 ms in order; linear interpolation over ranks 0..9.
    for (std::uint32_t ms = 1; ms <= 10; ++ms) telemetry.push(sampleWith(ms * 1000000u));

    const auto summary = telemetry.summarise(128, 48000.0);
    REQUIRE(summary.count == 10);
    CHECK(summary.renderP50Ms == Catch::Approx(5.5).margin(0.01));
    CHECK(summary.renderP99Ms == Catch::Approx(9.91).margin(0.02));
    CHECK(summary.renderMaxMs == Catch::Approx(10.0));
}

TEST_CASE("a slow block above the block period exceeds 100 percent", "[latency][telemetry]") {
    AudioTelemetry telemetry;
    for (int i = 0; i < 10; ++i) telemetry.push(sampleWith(50000));  // 0.05 ms
    telemetry.push(sampleWith(4000000));  // 4 ms, far above the 2.67 ms block

    // 128 frames at 48 kHz is 2.67 ms; a 4 ms block is >100 %.
    const auto summary = telemetry.summarise(128, 48000.0);
    CHECK(summary.renderWorstOverBlockPercent > 100.0);
}

TEST_CASE("flag bits are counted once per sample", "[latency][telemetry]") {
    AudioTelemetry telemetry;
    telemetry.push(sampleWith(1000, 256, AudioTelemetry::flagRenderMiss));
    telemetry.push(sampleWith(1000, 256, AudioTelemetry::flagRenderMiss
                                             | AudioTelemetry::flagOutputUnderrun));
    telemetry.push(sampleWith(1000, 256, AudioTelemetry::flagOversized));

    const auto summary = telemetry.summarise(128, 48000.0);
    CHECK(summary.renderMisses == 2);
    CHECK(summary.outputUnderruns == 1);
    CHECK(summary.oversizedBlocks == 1);
    CHECK(summary.inputUnderruns == 0);
}

TEST_CASE("the ring only reports the newest window", "[latency][telemetry]") {
    AudioTelemetry telemetry;
    telemetry.push(sampleWith(99000000));  // 99 ms, must age out
    for (int i = 0; i < AudioTelemetry::kCapacity; ++i) telemetry.push(sampleWith(1000000));

    const auto summary = telemetry.summarise(128, 48000.0);
    CHECK(summary.count == AudioTelemetry::kCapacity);
    CHECK(summary.renderMaxMs == Catch::Approx(1.0).margin(0.001));
}

TEST_CASE("a constant fill lands in one bucket and a ramp spreads", "[latency][telemetry]") {
    {
        AudioTelemetry telemetry;
        for (int i = 0; i < 8; ++i) telemetry.push(sampleWith(1000, 512));
        const auto summary = telemetry.summarise(128, 48000.0);
        CHECK(summary.outputFillBucketLowFrames == 512);
        CHECK(summary.outputFillBucketWidthFrames == 1);
        CHECK(summary.outputFillHistogram[0] == 8);
    }
    {
        AudioTelemetry telemetry;
        for (int i = 0; i < 160; ++i) telemetry.push(sampleWith(1000, i));
        const auto summary = telemetry.summarise(128, 48000.0);
        CHECK(summary.outputFillBucketLowFrames == 0);
        CHECK(summary.outputFillHistogram[0] > 0);
        CHECK(summary.outputFillHistogram[AudioTelemetry::kFillBuckets - 1] > 0);
        std::uint32_t total = 0;
        for (const auto bucket : summary.outputFillHistogram) total += bucket;
        CHECK(total == 160);
    }
}

TEST_CASE("a zero block or rate cannot divide by zero", "[latency][telemetry]") {
    AudioTelemetry telemetry;
    telemetry.push(sampleWith(1000000));
    const auto summary = telemetry.summarise(0, 0.0);
    CHECK(summary.renderWorstOverBlockPercent == 0.0);
    CHECK(summary.count == 1);
}

TEST_CASE("reset empties the window", "[latency][telemetry]") {
    AudioTelemetry telemetry;
    telemetry.push(sampleWith(1000000));
    telemetry.reset();
    CHECK(telemetry.summarise(128, 48000.0).count == 0);
}
