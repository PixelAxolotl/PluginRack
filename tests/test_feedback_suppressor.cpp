#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "pluginrack/FeedbackSuppressorEffect.h"
#include "pluginrack/SceneCodec.h"

namespace {
std::vector<float> sine(float hz, float amplitude, int frames, double sampleRate = 48000.0) {
    std::vector<float> samples(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        samples[static_cast<std::size_t>(i)] = amplitude * static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(hz)
                     * static_cast<double>(i) / sampleRate));
    }
    return samples;
}

float rms(const std::vector<float>& samples) {
    double sum = 0.0;
    for (float value : samples) sum += static_cast<double>(value) * value;
    return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}
} // namespace

TEST_CASE("feedback suppressor notches a ringing tone") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(1000.0f, 0.4f, 16384);
    auto right = left;
    const float inputRms = rms(left);
    // Real hosts call process() in small blocks; the analyzer runs between
    // blocks, so feed it the same way instead of one giant offline block.
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(count > 0);
    const auto found = std::find_if(notches.begin(), notches.begin() + count, [](float hz) {
        return std::fabs(hz - 1000.0f) < 20.0f;
    });
    REQUIRE(found != notches.begin() + count);

    REQUIRE(rms(left) < inputRms * 0.9f);
    REQUIRE(processor.gainReductionDb() > 0.0f);
}

TEST_CASE("feedback suppressor defaults match the current tuning") {
    const auto effect = pluginrack::makeDefaultFeedbackSuppressorEffect(7);
    REQUIRE(effect.id == 7u);
    REQUIRE(effect.bypassed == false);
    REQUIRE(pluginrack::feedbackSuppressorCutCount(effect.strength) == 8);
    REQUIRE(effect.attenuationDb == 24.0f);
    REQUIRE(effect.inputSensitivityDb == -43.0f);
    REQUIRE(effect.detectorHighPassHz == 20.0f);
    REQUIRE(effect.detectorLowPassHz == 20000.0f);
    REQUIRE(effect.gainReductionMultiplier == 4.0f);
    REQUIRE(effect.attackMs == 9.0f);
    REQUIRE(effect.holdMs == 90.0f);
    REQUIRE(effect.releaseMs == 10.0f);
    REQUIRE(effect.detectionDelayMs == 60.0f);
}

TEST_CASE("feedback suppressor depth follows ringing level") {
    const auto measuredDepth = [](float amplitude) {
        pluginrack::FeedbackSuppressorEffect effect;
        effect.detectionDelayMs = 0.0f;
        effect.strength = 0.5f;
        effect.attenuationDb = 24.0f;
        effect.gainReductionMultiplier = 1.0f;
        effect.inputSensitivityDb = -60.0f;
        pluginrack::FeedbackSuppressorProcessor processor;
        processor.prepare(48000.0);
        processor.setEffect(effect);

        auto left = sine(1000.0f, amplitude, 16384);
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, frames);
        }

        std::array<float, 8> notches{};
        std::array<float, 8> depths{};
        const int count = processor.copyActiveNotches(notches.data(), depths.data(),
                                                      static_cast<int>(notches.size()));
        REQUIRE(count > 0);
        return *std::max_element(depths.begin(), depths.begin() + count);
    };

    const float quietDepth = measuredDepth(0.02f);
    const float loudDepth = measuredDepth(0.4f);
    REQUIRE(quietDepth > 0.0f);
    REQUIRE(loudDepth > quietDepth + 1.0f);
}

TEST_CASE("feedback suppressor gain reduction multiplier scales cut depth") {
    const auto measuredDepth = [](float multiplier) {
        pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
        effect.strength = 0.5f;
        effect.attenuationDb = 24.0f;
        effect.inputSensitivityDb = -60.0f;
        effect.gainReductionMultiplier = multiplier;
        pluginrack::FeedbackSuppressorProcessor processor;
        processor.prepare(48000.0);
        processor.setEffect(effect);

        auto left = sine(1000.0f, 0.05f, 16384);
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, frames);
        }

        std::array<float, 8> notches{};
        std::array<float, 8> depths{};
        const int count = processor.copyActiveNotches(notches.data(), depths.data(),
                                                      static_cast<int>(notches.size()));
        REQUIRE(count > 0);
        return *std::max_element(depths.begin(), depths.begin() + count);
    };

    const float gentle = measuredDepth(0.5f);
    const float strong = measuredDepth(2.0f);
    REQUIRE(strong > gentle + 2.0f);
}

TEST_CASE("feedback suppressor detects a 68 Hz peak") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(1);
    effect.attenuationDb = 24.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 100.0f;
    effect.releaseMs = 100.0f;
    effect.inputSensitivityDb = -43.0f;
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 20000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(68.0f, 0.3f, 16384);
    auto right = left;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(count > 0);
    REQUIRE(std::fabs(notches[0] - 68.0f) < 10.0f);
}

TEST_CASE("feedback suppressor ignores broadband noise around a low peak") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 60.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(8);
    effect.attenuationDb = 24.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 100.0f;
    effect.releaseMs = 100.0f;
    effect.inputSensitivityDb = -43.0f;
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 20000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(68.0f, 0.2f, 16384);
    std::uint32_t rng = 0x12345678u;
    for (auto& sample : left) {
        rng = rng * 1664525u + 1013904223u;
        const float noise = (static_cast<float>(rng >> 8) / 8388607.5f) - 1.0f;
        sample += noise * 0.02f;
    }
    auto right = left;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(count == 1);
    REQUIRE(std::fabs(notches[0] - 68.0f) < 10.0f);
}

TEST_CASE("feedback suppressor prefers a 68 Hz peak over nearby harmonics") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(8);
    effect.attenuationDb = 24.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 100.0f;
    effect.releaseMs = 100.0f;
    effect.inputSensitivityDb = -43.0f;
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 20000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    std::vector<float> left(16384, 0.0f);
    for (std::size_t i = 0; i < left.size(); ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        left[i] = 0.20f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 68.0 * t))
                + 0.15f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 145.0 * t))
                + 0.12f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 220.0 * t))
                + 0.10f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 600.0 * t));
    }
    auto right = left;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(count > 0);
    REQUIRE(std::fabs(notches[0] - 68.0f) < 10.0f);
}

TEST_CASE("feedback suppressor detects a peak below the first full FFT bin") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(1);
    effect.attenuationDb = 12.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 100.0f;
    effect.releaseMs = 100.0f;
    effect.inputSensitivityDb = -80.0f;
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 8000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(35.0f, 0.3f, 16384);
    auto right = left;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(count > 0);
    REQUIRE(std::fabs(notches[0] - 35.0f) < 10.0f);
}

TEST_CASE("feedback suppressor prefers the strongest of two close peaks") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(4);
    effect.attenuationDb = 12.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 0.0f;
    effect.releaseMs = 100.0f;
    effect.inputSensitivityDb = -80.0f;
    effect.detectorHighPassHz = 200.0f;
    effect.detectorLowPassHz = 8000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(1000.0f, 0.4f, 16384);
    const auto weaker = sine(1040.0f, 0.2f, 16384);
    for (std::size_t i = 0; i < left.size(); ++i) left[i] += weaker[i];
    auto right = left;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(count == 1);
    REQUIRE(std::fabs(notches[0] - 1000.0f) < 10.0f);
}

TEST_CASE("feedback suppressor follows a low cut onto the moved peak") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(1);
    effect.attenuationDb = 12.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 100.0f;
    effect.releaseMs = 100.0f;
    effect.detectionDelayMs = 0.0f;
    effect.inputSensitivityDb = -80.0f;
    effect.detectorHighPassHz = 40.0f;
    effect.detectorLowPassHz = 8000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    const auto run = [&processor](float hz, int frames) {
        auto left = sine(hz, 0.2f, frames);
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int chunk = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, chunk);
        }
    };

    run(90.0f, 16384);
    std::array<float, 8> notches{};
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) == 1);
    REQUIRE(std::fabs(notches[0] - 90.0f) < 10.0f);

    // 100 Hz is inside the same ~23 Hz analyzer bin as 90 Hz. It should be
    // treated as the same cut, and the cut should glide onto the new peak
    // rather than staying on the old estimate or creating a second cut.
    run(100.0f, 4096);
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) == 1);
    REQUIRE(notches[0] > 90.0f);
    REQUIRE(notches[0] < 101.0f);
}

TEST_CASE("feedback suppressor waits before turning a new tone into a cut") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(1);
    effect.attenuationDb = 12.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 200.0f;
    effect.releaseMs = 100.0f;
    effect.detectionDelayMs = 120.0f;
    effect.inputSensitivityDb = -30.0f;
    effect.detectorHighPassHz = 40.0f;
    effect.detectorLowPassHz = 8000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(1200.0f, 0.05f, 4096);
    auto right = left;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int chunk = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, chunk);
    }
    std::array<float, 8> notches{};
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) == 0);
    REQUIRE(processor.gainReductionDb() == 0.0f);

    left = sine(1200.0f, 0.05f, 12288);
    right = left;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int chunk = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, chunk);
    }
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) == 1);
}

TEST_CASE("feedback suppressor analyzes inside a large host block") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(1000.0f, 0.4f, 16384);
    auto right = left;
    const float inputRms = rms(left);
    // A host is allowed to hand the processor one large block. It must still
    // run the detector part-way through rather than waiting for the next call.
    processor.process(left.data(), right.data(), static_cast<int>(left.size()));

    std::array<float, 8> notches{};
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) > 0);
    REQUIRE(rms(left) < inputRms * 0.9f);
}

TEST_CASE("feedback suppressor detector ignores tones below the high-pass") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    effect.detectorHighPassHz = 300.0f;
    effect.detectorLowPassHz = 8000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto input = sine(100.0f, 0.4f, 16384);
    auto left = input;
    auto right = input;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) == 0);
    REQUIRE(left == input);
}

TEST_CASE("feedback suppressor detector ignores tones above the low-pass") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    effect.detectorHighPassHz = 300.0f;
    effect.detectorLowPassHz = 4000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto input = sine(6000.0f, 0.4f, 16384);
    auto left = input;
    auto right = input;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    const int aboveCount = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(aboveCount == 0);
    REQUIRE(left == input);
}

TEST_CASE("feedback suppressor detector still finds tones inside its band") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    effect.detectorHighPassHz = 300.0f;
    effect.detectorLowPassHz = 4000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(1000.0f, 0.4f, 16384);
    auto right = left;
    const float inputRms = rms(left);
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) > 0);
    REQUIRE(rms(left) < inputRms * 0.9f);
}

TEST_CASE("feedback suppressor puts a low-frequency cut on the peak") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    effect.attenuationDb = 12.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 0.0f;
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 2000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(90.0f, 0.4f, 16384);
    auto right = left;
    const float inputRms = rms(left);
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(count > 0);
    const auto found = std::find_if(notches.begin(), notches.begin() + count, [](float hz) {
        return std::fabs(hz - 90.0f) < 10.0f;
    });
    REQUIRE(found != notches.begin() + count);
    REQUIRE(rms(left) < inputRms * 0.95f);
}

TEST_CASE("feedback suppressor finds a clipped low-frequency peak") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    effect.attenuationDb = 12.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 0.0f;
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 2000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(90.0f, 1.5f, 16384);
    for (auto& sample : left) sample = std::clamp(sample, -1.0f, 1.0f);
    auto right = left;
    const float inputRms = rms(left);
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(count > 0);
    const auto found = std::find_if(notches.begin(), notches.begin() + count, [](float hz) {
        return std::fabs(hz - 90.0f) < 10.0f;
    });
    REQUIRE(found != notches.begin() + count);
    REQUIRE(rms(left) < inputRms * 0.98f);
}

TEST_CASE("feedback suppressor ignores input below its sensitivity") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    effect.inputSensitivityDb = -20.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto input = sine(1000.0f, 0.02f, 16384);
    auto left = input;
    auto right = input;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    // The gate is exact: below the threshold the processor must not touch a
    // single sample and must not publish a notch.
    REQUIRE(left == input);
    REQUIRE(right == input);
    std::array<float, 8> notches{};
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) == 0);
    REQUIRE(processor.gainReductionDb() == 0.0f);
}

TEST_CASE("feedback suppressor processes input above its sensitivity") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    effect.inputSensitivityDb = -40.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto input = sine(1000.0f, 0.2f, 16384);
    auto left = input;
    auto right = input;
    const float inputRms = rms(input);
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    REQUIRE(rms(left) < inputRms * 0.9f);
    std::array<float, 8> notches{};
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) > 0);
}

TEST_CASE("feedback suppressor strength maps to a cut count") {
    REQUIRE(pluginrack::feedbackSuppressorCutCount(0.0f) == 0);
    REQUIRE(pluginrack::feedbackSuppressorCutCount(0.125f) == 1);
    REQUIRE(pluginrack::feedbackSuppressorCutCount(0.25f) == 2);
    REQUIRE(pluginrack::feedbackSuppressorCutCount(0.5f) == 4);
    REQUIRE(pluginrack::feedbackSuppressorCutCount(1.0f) == 8);
    REQUIRE(pluginrack::feedbackSuppressorStrengthForCuts(4) == 0.5f);
    REQUIRE(pluginrack::feedbackSuppressorNotchQ(24.0f)
            < pluginrack::feedbackSuppressorNotchQ(6.0f));
}

TEST_CASE("feedback suppressor notch Q rises with frequency") {
    const float low = pluginrack::feedbackSuppressorNotchQ(24.0f, 100.0f, 23.4f);
    const float mid = pluginrack::feedbackSuppressorNotchQ(24.0f, 1000.0f, 23.4f);
    const float high = pluginrack::feedbackSuppressorNotchQ(24.0f, 5000.0f, 23.4f);
    const float top = pluginrack::feedbackSuppressorNotchQ(24.0f, 10000.0f, 23.4f);
    REQUIRE(low < mid);
    REQUIRE(mid < high);
    REQUIRE(high < top);
    REQUIRE(top <= 48.0f);
}

TEST_CASE("feedback suppressor attenuation controls cut depth") {
    const auto run = [](float attenuationDb) {
        pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
        effect.strength = 0.5f;
        effect.attenuationDb = attenuationDb;
        effect.attackMs = 0.0f;
        effect.holdMs = 0.0f;
        pluginrack::FeedbackSuppressorProcessor processor;
        processor.prepare(48000.0);
        processor.setEffect(effect);
        auto left = sine(1000.0f, 0.4f, 16384);
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, frames);
        }
        return rms(left);
    };

    const float shallow = run(3.0f);
    const float deep = run(24.0f);
    REQUIRE(deep < shallow * 0.9f);
}

TEST_CASE("feedback suppressor attack delays a new cut") {
    const auto run = [](float attackMs) {
        pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
        effect.strength = 0.5f;
        effect.attenuationDb = 24.0f;
        effect.attackMs = attackMs;
        effect.holdMs = 0.0f;
        pluginrack::FeedbackSuppressorProcessor processor;
        processor.prepare(48000.0);
        processor.setEffect(effect);
        auto left = sine(1000.0f, 0.4f, 8192);
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, frames);
        }
        return rms(left);
    };

    REQUIRE(run(100.0f) > run(0.0f));
}

TEST_CASE("feedback suppressor hold keeps a cut after the ringing stops") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    effect.attenuationDb = 12.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 100.0f;
    effect.releaseMs = 20.0f;
    effect.detectorHighPassHz = 1000.0f;
    effect.detectorLowPassHz = 2000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto processTone = [&processor](float hz, int frames) {
        auto left = sine(hz, 0.4f, frames);
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int chunk = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, chunk);
        }
    };

    processTone(1500.0f, 4096);
    std::array<float, 8> notches{};
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) > 0);

    auto processSilence = [&processor](int frames) {
        std::vector<float> left(static_cast<std::size_t>(frames), 0.0f);
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int chunk = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, chunk);
        }
    };

    processSilence(2000);
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) > 0);

    processSilence(12000);
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) == 0);
}

TEST_CASE("feedback suppressor never exceeds requested cut count while holding") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(2);
    effect.attenuationDb = 12.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 2000.0f;
    effect.releaseMs = 200.0f;
    effect.detectorHighPassHz = 500.0f;
    effect.detectorLowPassHz = 3000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto processTones = [&processor](const std::vector<float>& freqs, int frames) {
        std::vector<float> left(static_cast<std::size_t>(frames), 0.0f);
        for (float hz : freqs) {
            const auto tone = sine(hz, 0.2f, frames);
            for (std::size_t i = 0; i < left.size(); ++i) left[i] += tone[i];
        }
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int chunk = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, chunk);
        }
    };

    processTones({1000.0f, 1500.0f}, 16384);
    processTones({2000.0f, 2500.0f}, 16384);

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(count <= 2);
}

TEST_CASE("feedback suppressor publishes attack and release depth") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(1);
    effect.attenuationDb = 12.0f;
    effect.attackMs = 200.0f;
    effect.holdMs = 0.0f;
    effect.releaseMs = 400.0f;
    effect.inputSensitivityDb = -60.0f;
    effect.detectorHighPassHz = 500.0f;
    effect.detectorLowPassHz = 3000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto processTone = [&processor](float hz, int frames) {
        auto left = sine(hz, 0.4f, frames);
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int chunk = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, chunk);
        }
    };
    auto processSilence = [&processor](int frames) {
        std::vector<float> left(static_cast<std::size_t>(frames), 0.0f);
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int chunk = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, chunk);
        }
    };

    std::array<float, 8> freqs{};
    std::array<float, 8> depths{};

    processTone(1500.0f, 4096);
    REQUIRE(processor.copyActiveNotches(freqs.data(), depths.data(),
                                        static_cast<int>(freqs.size())) > 0);
    const float earlyDepth = depths[0];
    REQUIRE(earlyDepth > 0.0f);

    processTone(1500.0f, 24000);
    REQUIRE(processor.copyActiveNotches(freqs.data(), depths.data(),
                                        static_cast<int>(freqs.size())) > 0);
    const float fullDepth = depths[0];
    REQUIRE(fullDepth > earlyDepth + 1.0f);

    processSilence(4800);
    REQUIRE(processor.copyActiveNotches(freqs.data(), depths.data(),
                                        static_cast<int>(freqs.size())) > 0);
    REQUIRE(depths[0] > 0.0f);
    REQUIRE(depths[0] < fullDepth - 1.0f);

    processSilence(24000);
    REQUIRE(processor.copyActiveNotches(freqs.data(), depths.data(),
                                        static_cast<int>(freqs.size())) == 0);
}

TEST_CASE("feedback suppressor holds and releases when the input falls below the gate") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = 0.5f;
    effect.attenuationDb = 12.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 100.0f;
    effect.releaseMs = 200.0f;
    effect.inputSensitivityDb = -20.0f;
    effect.detectorHighPassHz = 500.0f;
    effect.detectorLowPassHz = 3000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto processBlock = [&processor](std::vector<float>& left, std::vector<float>& right) {
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int chunk = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, chunk);
        }
    };

    auto tone = sine(1500.0f, 0.4f, 8192);
    auto toneRight = tone;
    processBlock(tone, toneRight);

    std::array<float, 8> notches{};
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) > 0);

    std::vector<float> quiet(7200, 0.0f);
    auto quietRight = quiet;
    processBlock(quiet, quietRight);
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) > 0);

    std::vector<float> longQuiet(24000, 0.0f);
    auto longQuietRight = longQuiet;
    processBlock(longQuiet, longQuietRight);
    REQUIRE(processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) == 0);
}


TEST_CASE("feedback suppressor ignores DC when finding a low peak") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(8);
    effect.attenuationDb = 24.0f;
    effect.gainReductionMultiplier = 1.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 100.0f;
    effect.releaseMs = 100.0f;
    effect.inputSensitivityDb = -43.0f;
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 20000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(68.0f, 0.2f, 32768);
    for (float& sample : left) sample += 0.45f;
    auto right = left;
    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int count = static_cast<int>(
            std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, count);
    }

    std::array<float, 8> freqs{};
    const int count = processor.copyActiveNotches(freqs.data(),
                                                  static_cast<int>(freqs.size()));
    REQUIRE(count >= 1);
    REQUIRE(freqs[0] > 55.0f);
    REQUIRE(freqs[0] < 85.0f);
}

TEST_CASE("feedback suppressor glides an existing cut onto a moved peak") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(1);
    effect.attenuationDb = 24.0f;
    effect.gainReductionMultiplier = 1.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 500.0f;
    effect.releaseMs = 100.0f;
    effect.inputSensitivityDb = -43.0f;
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 20000.0f;
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto first = sine(55.0f, 0.2f, 16384);
    auto second = sine(60.0f, 0.2f, 16384);
    auto firstRight = first;
    auto secondRight = second;
    for (std::size_t offset = 0; offset < first.size(); offset += 512) {
        const int count = static_cast<int>(
            std::min<std::size_t>(512, first.size() - offset));
        processor.process(first.data() + offset, firstRight.data() + offset, count);
    }
    for (std::size_t offset = 0; offset < second.size(); offset += 512) {
        const int count = static_cast<int>(
            std::min<std::size_t>(512, second.size() - offset));
        processor.process(second.data() + offset, secondRight.data() + offset, count);
    }

    std::array<float, 8> freqs{};
    const int count = processor.copyActiveNotches(freqs.data(),
                                                  static_cast<int>(freqs.size()));
    REQUIRE(count >= 1);
    REQUIRE(freqs[0] > 57.0f);
    REQUIRE(freqs[0] < 63.0f);
}

TEST_CASE("feedback suppressor processor is copyable for engine rebuilds") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    pluginrack::FeedbackSuppressorProcessor original;
    original.prepare(48000.0);
    original.setEffect(effect);
    auto left = sine(1000.0f, 0.3f, 8192);
    auto right = left;
    original.process(left.data(), right.data(), static_cast<int>(left.size()));

    pluginrack::FeedbackSuppressorProcessor copy = original;
    std::array<float, 8> notches{};
    REQUIRE(copy.copyActiveNotches(notches.data(), static_cast<int>(notches.size()))
            == original.copyActiveNotches(notches.data(), static_cast<int>(notches.size())));
}

TEST_CASE("feedback suppressor effect settings serialize") {
    pluginrack::Scene scene;
    pluginrack::Channel channel;
    channel.id = 1;
    channel.feedbackSuppressorEffects = {pluginrack::makeDefaultFeedbackSuppressorEffect(7)};
    channel.feedbackSuppressorEffects.front().attenuationDb = 18.0f;
    channel.feedbackSuppressorEffects.front().gainReductionMultiplier = 1.75f;
    channel.feedbackSuppressorEffects.front().attackMs = 25.0f;
    channel.feedbackSuppressorEffects.front().holdMs = 750.0f;
    channel.feedbackSuppressorEffects.front().releaseMs = 180.0f;
    channel.feedbackSuppressorEffects.front().detectionDelayMs = 240.0f;
    channel.feedbackSuppressorEffects.front().inputSensitivityDb = -33.0f;
    channel.feedbackSuppressorEffects.front().detectorHighPassHz = 250.0f;
    channel.feedbackSuppressorEffects.front().detectorLowPassHz = 5000.0f;
    channel.feedbackSuppressorRtaOffsetDb = -6.0f;
    channel.feedbackSuppressorRtaWeighting = 2;
    channel.chain = {{pluginrack::ChainItemType::FeedbackSuppressor, 7}};
    scene.channels.push_back(channel);
    scene.outputs.push_back({1, "Out"});
    const auto json = pluginrack::toJson(scene);
    REQUIRE(json.find("FeedbackSuppressor") != std::string::npos);
    const auto loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    const auto& strip = loaded.scene->channels.front();
    REQUIRE(strip.feedbackSuppressorEffects.front().id == 7);
    REQUIRE(strip.feedbackSuppressorEffects.front().attenuationDb == 18.0f);
    REQUIRE(strip.feedbackSuppressorEffects.front().gainReductionMultiplier == 1.75f);
    REQUIRE(strip.feedbackSuppressorEffects.front().attackMs == 25.0f);
    REQUIRE(strip.feedbackSuppressorEffects.front().holdMs == 750.0f);
    REQUIRE(strip.feedbackSuppressorEffects.front().releaseMs == 180.0f);
    REQUIRE(strip.feedbackSuppressorEffects.front().detectionDelayMs == 240.0f);
    REQUIRE(strip.feedbackSuppressorEffects.front().inputSensitivityDb == -33.0f);
    REQUIRE(strip.feedbackSuppressorEffects.front().detectorHighPassHz == 250.0f);
    REQUIRE(strip.feedbackSuppressorEffects.front().detectorLowPassHz == 5000.0f);
    REQUIRE(strip.feedbackSuppressorRtaOffsetDb == -6.0f);
    REQUIRE(strip.feedbackSuppressorRtaWeighting == 2);
}


TEST_CASE("feedback suppressor detects a low peak under louder high-frequency content") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 60.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(8);
    effect.attenuationDb = 24.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 100.0f;
    effect.releaseMs = 100.0f;
    effect.inputSensitivityDb = -43.0f;
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 20000.0f;

    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    auto left = sine(68.0f, 0.02f, 32768);
    const auto high = sine(5000.0f, 0.5f, 32768);
    for (std::size_t i = 0; i < left.size(); ++i) left[i] += high[i];
    auto right = left;

    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    bool foundLow = false;
    for (int i = 0; i < count; ++i) {
        if (std::fabs(notches[i] - 68.0f) < 10.0f) foundLow = true;
    }
    REQUIRE(foundLow);
}

TEST_CASE("feedback suppressor separates three close low-frequency bands") {
    pluginrack::FeedbackSuppressorEffect effect;
    effect.detectionDelayMs = 0.0f;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(8);
    effect.attenuationDb = 24.0f;
    effect.gainReductionMultiplier = 1.0f;
    effect.attackMs = 0.0f;
    effect.holdMs = 200.0f;
    effect.releaseMs = 100.0f;
    effect.inputSensitivityDb = -43.0f;
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 20000.0f;

    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(effect);

    constexpr int frames = 48000;
    auto left = sine(70.0f, 0.08f, frames);
    const auto second = sine(90.0f, 0.08f, frames);
    const auto third = sine(110.0f, 0.08f, frames);
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] += second[i] + third[i];
    }
    auto right = left;

    for (std::size_t offset = 0; offset < left.size(); offset += 512) {
        const int count = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, count);
    }

    std::array<float, 8> notches{};
    const int count = processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size()));
    REQUIRE(count == 3);
    const auto hasNear = [&notches, count](float hz) {
        for (int i = 0; i < count; ++i) {
            if (std::fabs(notches[static_cast<std::size_t>(i)] - hz) < 3.0f) return true;
        }
        return false;
    };
    REQUIRE(hasNear(70.0f));
    REQUIRE(hasNear(90.0f));
    REQUIRE(hasNear(110.0f));
}

TEST_CASE("feedback suppressor confirms a loud peak sooner than a quiet one") {
    const auto hasCutAfter = [](float amplitude) {
        pluginrack::FeedbackSuppressorEffect effect;
        effect.detectionDelayMs = 120.0f;
        effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(8);
        effect.attenuationDb = 24.0f;
        effect.gainReductionMultiplier = 1.0f;
        effect.attackMs = 0.0f;
        effect.holdMs = 100.0f;
        effect.releaseMs = 100.0f;
        effect.inputSensitivityDb = -45.0f;
        effect.detectorHighPassHz = 20.0f;
        effect.detectorLowPassHz = 20000.0f;

        pluginrack::FeedbackSuppressorProcessor processor;
        processor.prepare(48000.0);
        processor.setEffect(effect);

        auto left = sine(100.0f, amplitude, 8192 + 2560);
        auto right = left;
        for (std::size_t offset = 0; offset < left.size(); offset += 512) {
            const int count = static_cast<int>(std::min<std::size_t>(512, left.size() - offset));
            processor.process(left.data() + offset, right.data() + offset, count);
        }

        std::array<float, 8> notches{};
        return processor.copyActiveNotches(notches.data(), static_cast<int>(notches.size())) > 0;
    };

    REQUIRE(hasCutAfter(0.5f));
    REQUIRE_FALSE(hasCutAfter(0.01f));
}
