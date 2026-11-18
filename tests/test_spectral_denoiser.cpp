#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "pluginrack/SceneCodec.h"
#include "pluginrack/SceneValidator.h"
#include "pluginrack/MixerEngine.h"
#include "pluginrack/SpectralDenoiserEffect.h"

namespace {

constexpr double kSampleRate = 48000.0;

std::vector<float> noise(std::size_t size, std::uint32_t seed = 0x12345678u) {
    std::vector<float> samples(size, 0.0f);
    std::uint32_t state = seed;
    for (auto& sample : samples) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        sample = static_cast<float>(state) / static_cast<float>(std::numeric_limits<std::uint32_t>::max())
            * 2.0f - 1.0f;
    }
    return samples;
}

float rms(const std::vector<float>& samples, std::size_t begin) {
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t i = begin; i < samples.size(); ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
        ++count;
    }
    return count == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

void processInBlocks(pluginrack::SpectralDenoiserProcessor& processor, std::vector<float>& left,
                     std::vector<float>& right, int blockSize = 512) {
    for (std::size_t offset = 0; offset < left.size(); offset += static_cast<std::size_t>(blockSize)) {
        const int frames = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(blockSize), left.size() - offset));
        processor.process(left.data() + offset, right.data() + offset, frames);
    }
}

} // namespace

TEST_CASE("spectral denoiser reports one frame of latency") {
    REQUIRE(pluginrack::SpectralDenoiserProcessor::latencySamples() == 512);

    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
    effect.reductionDb = 0.0f;
    pluginrack::SpectralDenoiserProcessor processor;
    processor.prepare(kSampleRate);
    processor.setEffect(effect);

    auto input = noise(8192);
    auto left = input;
    auto right = input;
    processInBlocks(processor, left, right);

    for (std::size_t i = 1024; i < input.size(); ++i) {
        REQUIRE(left[i] == Catch::Approx(input[i - 512]).margin(1.0e-4f));
        REQUIRE(right[i] == Catch::Approx(input[i - 512]).margin(1.0e-4f));
    }
}

TEST_CASE("spectral denoiser latency does not depend on the host block size") {
    for (const int blockSize : {1, 17, 64, 128, 333, 1024, 4096}) {
        auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
        effect.reductionDb = 0.0f;
        pluginrack::SpectralDenoiserProcessor processor;
        processor.prepare(kSampleRate);
        processor.setEffect(effect);

        auto input = noise(4096, 0x0badf00du);
        auto left = input;
        auto right = input;
        processInBlocks(processor, left, right, blockSize);

        for (std::size_t i = 1024; i < input.size(); ++i) {
            REQUIRE(left[i] == Catch::Approx(input[i - 512]).margin(1.0e-4f));
        }
    }
}

TEST_CASE("spectral denoiser learned profile protects a tone above the noise") {
    pluginrack::SpectralDenoiserEffect learning = pluginrack::makeDefaultSpectralDenoiserEffect();
    pluginrack::SpectralDenoiserProcessor learner;
    learner.prepare(kSampleRate);
    learner.setEffect(learning);
    learner.startLearning();

    auto trainingLeft = noise(static_cast<std::size_t>(kSampleRate * 1.5), 0x2468ace0u);
    for (auto& sample : trainingLeft) sample *= 0.02f;
    auto trainingRight = trainingLeft;
    processInBlocks(learner, trainingLeft, trainingRight);

    std::vector<float> profile;
    double profileRate = 0.0;
    REQUIRE(learner.takeLearnedProfile(profile, profileRate));
    REQUIRE(profileRate == Catch::Approx(kSampleRate));
    REQUIRE(profile.size() == static_cast<std::size_t>(pluginrack::kSpectralDenoiserBinCount));

    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
    effect.mode = pluginrack::SpectralDenoiserMode::Learn;
    effect.reductionDb = 18.0f;
    effect.noiseProfile = profile;
    effect.profileSampleRate = profileRate;
    pluginrack::SpectralDenoiserProcessor processor;
    processor.prepare(kSampleRate);
    processor.setEffect(effect);

    std::vector<float> left(static_cast<std::size_t>(kSampleRate * 2.0));
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = 0.5f * std::sin(2.0 * 3.14159265358979323846 * 1000.0
                                  * static_cast<double>(i) / kSampleRate);
    }
    auto right = left;
    processInBlocks(processor, left, right);

    REQUIRE(rms(left, left.size() / 2) > 0.30f);
}

TEST_CASE("spectral denoiser remaps a learned profile to another sample rate") {
    pluginrack::SpectralDenoiserProcessor learner;
    learner.prepare(44100.0);
    learner.setEffect(pluginrack::makeDefaultSpectralDenoiserEffect());
    learner.startLearning();

    auto trainingLeft = noise(static_cast<std::size_t>(44100.0 * 1.5), 0x13579bdfu);
    for (auto& sample : trainingLeft) sample *= 0.02f;
    auto trainingRight = trainingLeft;
    processInBlocks(learner, trainingLeft, trainingRight);

    std::vector<float> profile;
    double profileRate = 0.0;
    REQUIRE(learner.takeLearnedProfile(profile, profileRate));
    REQUIRE(profileRate == Catch::Approx(44100.0));

    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
    effect.mode = pluginrack::SpectralDenoiserMode::Learn;
    effect.reductionDb = 24.0f;
    effect.noiseProfile = profile;
    effect.profileSampleRate = profileRate;
    pluginrack::SpectralDenoiserProcessor processor;
    processor.prepare(kSampleRate);
    processor.setEffect(effect);

    auto input = noise(static_cast<std::size_t>(kSampleRate * 1.5), 0x2468ace0u);
    for (auto& sample : input) sample *= 0.02f;
    auto left = input;
    auto right = input;
    processInBlocks(processor, left, right);

    REQUIRE(rms(left, input.size() / 2) < rms(input, input.size() / 2) * 0.6f);
}

TEST_CASE("spectral denoiser bypass keeps the delay") {
    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
    effect.bypassed = true;
    pluginrack::SpectralDenoiserProcessor processor;
    processor.prepare(kSampleRate);
    processor.setEffect(effect);

    auto input = noise(8192, 0xabcdef01u);
    auto left = input;
    auto right = input;
    processInBlocks(processor, left, right);

    for (std::size_t i = 1024; i < input.size(); ++i) {
        REQUIRE(left[i] == Catch::Approx(input[i - 512]).margin(1.0e-4f));
    }
}

TEST_CASE("spectral denoiser reduces steady noise") {
    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
    effect.reductionDb = 24.0f;
    effect.mode = pluginrack::SpectralDenoiserMode::Adaptive;
    pluginrack::SpectralDenoiserProcessor processor;
    processor.prepare(kSampleRate);
    processor.setEffect(effect);

    auto input = noise(96000, 0x0badc0deu);
    for (auto& sample : input) {
        sample *= 0.1f;
    }
    auto left = input;
    auto right = input;
    processInBlocks(processor, left, right);

    const float inputLevel = rms(input, 48000);
    const float outputLevel = rms(left, 48000);
    REQUIRE(outputLevel < inputLevel * 0.5f);
    REQUIRE(processor.currentReductionDb() > 6.0f);
}

TEST_CASE("spectral denoiser publishes band gain reduction") {
    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
    effect.reductionDb = 24.0f;
    effect.mode = pluginrack::SpectralDenoiserMode::Adaptive;
    pluginrack::SpectralDenoiserProcessor processor;
    processor.prepare(kSampleRate);
    processor.setEffect(effect);

    auto input = noise(96000, 0x13572468u);
    for (auto& sample : input) {
        sample *= 0.1f;
    }
    auto left = input;
    auto right = input;
    processInBlocks(processor, left, right);

    float peak = 0.0f;
    for (int band = 0; band < pluginrack::kSpectralDenoiserTrimBandCount; ++band) {
        const float reduction = processor.bandReductionDb(band);
        REQUIRE(std::isfinite(reduction));
        REQUIRE(reduction >= 0.0f);
        REQUIRE(reduction <= pluginrack::kSpectralDenoiserMaxReductionDb);
        peak = std::max(peak, reduction);
    }
    REQUIRE(peak > 6.0f);

    // Low log bands contain no bin of their own; the meter samples the gain
    // curve so every bar stays populated.
    for (int band = 0; band < pluginrack::kSpectralDenoiserTrimBandCount; ++band) {
        REQUIRE(processor.bandReductionDb(band) > 0.5f);
    }

    // Out-of-range bands read as silence rather than touching memory.
    REQUIRE(processor.bandReductionDb(-1) == 0.0f);
    REQUIRE(processor.bandReductionDb(pluginrack::kSpectralDenoiserTrimBandCount) == 0.0f);

    processor.reset();
    for (int band = 0; band < pluginrack::kSpectralDenoiserTrimBandCount; ++band) {
        REQUIRE(processor.bandReductionDb(band) == 0.0f);
    }
}

TEST_CASE("spectral denoiser interpolates trims between bands") {
    std::vector<float> trims(pluginrack::kSpectralDenoiserTrimBandCount, 0.0f);
    trims[4] = 12.0f;

    const double nyquist = 24000.0;
    const double low = pluginrack::kSpectralDenoiserTrimLowHz;
    const double ratio = nyquist / low;
    const auto frequencyAt = [&](double position) {
        return low * std::pow(ratio, position / pluginrack::kSpectralDenoiserTrimBandCount);
    };

    // Each band keeps its own value at its centre.
    REQUIRE(pluginrack::spectralDenoiserTrimDbAt(trims, frequencyAt(4.5), nyquist)
            == Catch::Approx(12.0f));
    REQUIRE(pluginrack::spectralDenoiserTrimDbAt(trims, frequencyAt(5.5), nyquist)
            == Catch::Approx(0.0f));

    // The curve blends across the boundary between bands.
    REQUIRE(pluginrack::spectralDenoiserTrimDbAt(trims, frequencyAt(5.0), nyquist)
            == Catch::Approx(6.0f));

    REQUIRE(pluginrack::spectralDenoiserTrimDbAt({}, frequencyAt(4.5), nyquist) == 0.0f);
}

TEST_CASE("spectral denoiser learns a noise profile") {
    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
    effect.mode = pluginrack::SpectralDenoiserMode::Learn;
    pluginrack::SpectralDenoiserProcessor processor;
    processor.prepare(kSampleRate);
    processor.setEffect(effect);

    auto left = noise(static_cast<std::size_t>(kSampleRate * 1.5), 0x5eed1234u);
    for (auto& sample : left) {
        sample *= 0.05f;
    }
    auto right = left;
    processor.startLearning();
    REQUIRE(processor.isLearning());
    processInBlocks(processor, left, right);

    REQUIRE_FALSE(processor.isLearning());
    REQUIRE(processor.learnProgress() == Catch::Approx(1.0f));

    std::vector<float> profile;
    double profileRate = 0.0;
    REQUIRE(processor.takeLearnedProfile(profile, profileRate));
    REQUIRE(profileRate == Catch::Approx(kSampleRate));
    REQUIRE(profile.size() == static_cast<std::size_t>(pluginrack::kSpectralDenoiserBinCount));
    double total = 0.0;
    for (float bin : profile) {
        REQUIRE(std::isfinite(bin));
        REQUIRE(bin >= 0.0f);
        total += bin;
    }
    REQUIRE(total > 0.0);
    REQUIRE_FALSE(processor.takeLearnedProfile(profile, profileRate));
}

TEST_CASE("spectral denoiser band trim changes how hard a band is cut") {
    pluginrack::SpectralDenoiserProcessor learner;
    learner.prepare(kSampleRate);
    learner.setEffect(pluginrack::makeDefaultSpectralDenoiserEffect());
    learner.startLearning();

    auto training = noise(static_cast<std::size_t>(kSampleRate * 1.5), 0x0ddc0ffe);
    for (auto& sample : training) sample *= 0.02f;
    auto trainingRight = training;
    processInBlocks(learner, training, trainingRight);

    std::vector<float> profile;
    double profileRate = 0.0;
    REQUIRE(learner.takeLearnedProfile(profile, profileRate));

    auto render = [&](float trimDb) {
        auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
        effect.mode = pluginrack::SpectralDenoiserMode::Learn;
        effect.reductionDb = 18.0f;
        effect.noiseProfile = profile;
        effect.profileSampleRate = profileRate;
        effect.profileTrimDb.assign(pluginrack::kSpectralDenoiserTrimBandCount, trimDb);

        pluginrack::SpectralDenoiserProcessor processor;
        processor.prepare(kSampleRate);
        processor.setEffect(effect);

        const auto size = static_cast<std::size_t>(kSampleRate * 1.5);
        std::vector<float> left(size);
        const auto signal = noise(size, 0x0ddc0ffe);
        for (std::size_t i = 0; i < size; ++i) {
            left[i] = 0.04f * signal[i];
        }
        auto right = left;
        processInBlocks(processor, left, right);

        return rms(left, size / 2);
    };

    const float protectedNoise = render(pluginrack::kSpectralDenoiserMinTrimDb);
    const float trimmedNoise = render(pluginrack::kSpectralDenoiserMaxTrimDb);
    REQUIRE(trimmedNoise < protectedNoise * 0.5f);
}

TEST_CASE("spectral denoiser threshold offset changes how hard noise is cut") {
    pluginrack::SpectralDenoiserProcessor learner;
    learner.prepare(kSampleRate);
    learner.setEffect(pluginrack::makeDefaultSpectralDenoiserEffect());
    learner.startLearning();

    auto training = noise(static_cast<std::size_t>(kSampleRate * 1.5), 0x0ddc0ffe);
    for (auto& sample : training) sample *= 0.02f;
    auto trainingRight = training;
    processInBlocks(learner, training, trainingRight);

    std::vector<float> profile;
    double profileRate = 0.0;
    REQUIRE(learner.takeLearnedProfile(profile, profileRate));

    auto render = [&](float offsetDb) {
        auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
        effect.mode = pluginrack::SpectralDenoiserMode::Learn;
        effect.reductionDb = 18.0f;
        effect.noiseProfile = profile;
        effect.profileSampleRate = profileRate;
        effect.thresholdOffsetDb = offsetDb;

        pluginrack::SpectralDenoiserProcessor processor;
        processor.prepare(kSampleRate);
        processor.setEffect(effect);

        const auto size = static_cast<std::size_t>(kSampleRate * 1.5);
        std::vector<float> left(size);
        const auto signal = noise(size, 0x0ddc0ffe);
        for (std::size_t i = 0; i < size; ++i) {
            left[i] = 0.04f * signal[i];
        }
        auto right = left;
        processInBlocks(processor, left, right);

        return rms(left, size / 2);
    };

    const float protectedNoise = render(pluginrack::kSpectralDenoiserMinThresholdOffsetDb);
    const float trimmedNoise = render(pluginrack::kSpectralDenoiserMaxThresholdOffsetDb);
    REQUIRE(trimmedNoise < protectedNoise * 0.5f);
}

TEST_CASE("spectral denoiser effect settings serialize") {
    pluginrack::SpectralDenoiserEffect effect = pluginrack::makeDefaultSpectralDenoiserEffect(9);
    effect.mode = pluginrack::SpectralDenoiserMode::Learn;
    effect.reductionDb = 18.0f;
    effect.noiseProfile.assign(
        static_cast<std::size_t>(pluginrack::kSpectralDenoiserBinCount), 0.5f);
    effect.profileSampleRate = 44100.0;
    effect.profileTrimDb.assign(pluginrack::kSpectralDenoiserTrimBandCount, 0.0f);
    effect.profileTrimDb[3] = 6.5f;
    effect.thresholdOffsetDb = 7.5f;

    pluginrack::Scene scene;
    scene.schemaVersion = pluginrack::kSchemaVersion;
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;
    pluginrack::Channel channel;
    channel.id = 1;
    channel.spectralDenoiserEffects = {effect};
    channel.chain = {{pluginrack::ChainItemType::SpectralDenoiser, 9}};
    scene.channels.push_back(channel);
    scene.outputs.push_back({1, "Out"});

    const auto json = pluginrack::toJson(scene);
    REQUIRE(json.find("SpectralDenoiser") != std::string::npos);
    REQUIRE(json.find("noiseProfile") != std::string::npos);
    REQUIRE(json.find("profileTrimDb") != std::string::npos);
    REQUIRE(json.find("thresholdOffsetDb") != std::string::npos);

    const auto loaded = pluginrack::fromJson(json);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.scene.has_value());
    const auto& restored = loaded.scene->channels.front().spectralDenoiserEffects.front();
    REQUIRE(restored.id == 9);
    REQUIRE(restored.mode == pluginrack::SpectralDenoiserMode::Learn);
    REQUIRE(restored.reductionDb == Catch::Approx(18.0f));
    REQUIRE(restored.profileSampleRate == Catch::Approx(44100.0));
    REQUIRE(restored.noiseProfile.size() == static_cast<std::size_t>(pluginrack::kSpectralDenoiserBinCount));
    REQUIRE(restored.noiseProfile.front() == Catch::Approx(0.5f));
    REQUIRE(restored.profileTrimDb.size()
            == static_cast<std::size_t>(pluginrack::kSpectralDenoiserTrimBandCount));
    REQUIRE(restored.profileTrimDb[3] == Catch::Approx(6.5f));
    REQUIRE(restored.thresholdOffsetDb == Catch::Approx(7.5f));
}

TEST_CASE("spectral denoiser validator rejects bad settings") {
    pluginrack::Scene scene;
    scene.schemaVersion = pluginrack::kSchemaVersion;
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;
    pluginrack::Channel channel;
    channel.id = 1;
    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect(2);
    effect.reductionDb = 99.0f;
    effect.noiseProfile = {0.1f, 0.2f};
    effect.profileSampleRate = 48000.0;
    channel.spectralDenoiserEffects = {effect};
    scene.channels.push_back(channel);
    scene.outputs.push_back({1, "Out"});

    REQUIRE_FALSE(pluginrack::validate(scene).empty());

    scene.channels.front().spectralDenoiserEffects.front() =
        pluginrack::makeDefaultSpectralDenoiserEffect(2);
    REQUIRE(pluginrack::validate(scene).empty());

    auto trimmed = pluginrack::makeDefaultSpectralDenoiserEffect(2);
    trimmed.profileTrimDb = {1.0f, 2.0f};
    scene.channels.front().spectralDenoiserEffects = {trimmed};
    REQUIRE_FALSE(pluginrack::validate(scene).empty());

    trimmed.profileTrimDb.assign(pluginrack::kSpectralDenoiserTrimBandCount, 0.0f);
    trimmed.profileTrimDb[7] = 99.0f;
    scene.channels.front().spectralDenoiserEffects = {trimmed};
    REQUIRE_FALSE(pluginrack::validate(scene).empty());

    trimmed.profileTrimDb[7] = -6.0f;
    scene.channels.front().spectralDenoiserEffects = {trimmed};
    REQUIRE(pluginrack::validate(scene).empty());

    auto offset = pluginrack::makeDefaultSpectralDenoiserEffect(2);
    offset.thresholdOffsetDb = 99.0f;
    scene.channels.front().spectralDenoiserEffects = {offset};
    REQUIRE_FALSE(pluginrack::validate(scene).empty());

    offset.thresholdOffsetDb = -6.0f;
    scene.channels.front().spectralDenoiserEffects = {offset};
    REQUIRE(pluginrack::validate(scene).empty());
}

TEST_CASE("spectral denoiser runs in the mixer chain") {
    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect(1);
    effect.reductionDb = 24.0f;

    pluginrack::Scene scene;
    scene.schemaVersion = pluginrack::kSchemaVersion;
    scene.sampleRate = 48000;
    scene.bufferFrames = 256;
    pluginrack::Channel channel;
    channel.id = 1;
    channel.layout = pluginrack::ChannelLayout::Mono;
    channel.spectralDenoiserEffects = {effect};
    channel.chain = {{pluginrack::ChainItemType::SpectralDenoiser, 1}};
    scene.channels.push_back(channel);
    scene.outputs.push_back({1, "Out"});
    scene.routes.push_back({1, 1, 0.0f});

    pluginrack::MixerEngine engine;
    std::string error;
    REQUIRE(engine.prepare(scene, 256, &error));

    auto input = noise(48000, 0x77777777u);
    for (auto& sample : input) {
        sample *= 0.1f;
    }
    std::vector<float> outputLeft(input.size(), 0.0f);
    std::vector<float> outputRight(input.size(), 0.0f);

    constexpr int blockFrames = 256;
    for (std::size_t offset = 0; offset < input.size(); offset += blockFrames) {
        const int frames = static_cast<int>(std::min<std::size_t>(
            blockFrames, input.size() - offset));
        float* inputChannels[2] = {input.data() + offset, input.data() + offset};
        float* outputChannels[2] = {outputLeft.data() + offset, outputRight.data() + offset};
        pluginrack::AudioBlock inputs[] = {{inputChannels, 2, frames}};
        pluginrack::AudioBlock outputs[] = {{outputChannels, 2, frames}};
        engine.render(inputs, outputs);
    }

    const float inputLevel = rms(input, 24000);
    const float outputLevel = rms(outputLeft, 24000);
    REQUIRE(outputLevel < inputLevel * 0.5f);
}

TEST_CASE("spectral denoiser keeps non-finite input out of the chain") {
    auto effect = pluginrack::makeDefaultSpectralDenoiserEffect();
    pluginrack::SpectralDenoiserProcessor processor;
    processor.prepare(kSampleRate);
    processor.setEffect(effect);

    std::vector<float> left(4096, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> right(4096, std::numeric_limits<float>::infinity());
    processInBlocks(processor, left, right);

    for (float sample : left) {
        REQUIRE(std::isfinite(sample));
    }
    for (float sample : right) {
        REQUIRE(std::isfinite(sample));
    }
}
