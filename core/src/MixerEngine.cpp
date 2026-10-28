#include "pluginrack/MixerEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "pluginrack/SceneValidator.h"

namespace pluginrack {

namespace {

constexpr float kMinAudibleGainDb = -60.0f;
constexpr float kPi = 3.14159265358979323846f;

float dbToLinear(float db) {
    if (db <= kMinAudibleGainDb) {
        return 0.0f;
    }
    return std::pow(10.0f, db / 20.0f);
}

int clampFrames(int frames, int maxFrames) noexcept {
    if (frames < 0) {
        return 0;
    }
    if (frames > maxFrames) {
        return maxFrames;
    }
    return frames;
}

} // namespace

bool MixerEngine::prepare(const Scene& scene, int maxBlockFrames, std::string* error) {
    const std::vector<SceneError> errors = validate(scene);
    if (!errors.empty()) {
        if (error != nullptr) {
            *error = errors.front().message;
        }
        return false;
    }
    if (scene.outputs.empty()) {
        if (error != nullptr) {
            *error = "scene has no outputs";
        }
        return false;
    }

    std::unordered_map<std::uint32_t, std::size_t> channelIndex;
    channelIndex.reserve(scene.channels.size() * 2 + 1);
    std::unordered_map<std::uint32_t, std::size_t> outputIndex;
    outputIndex.reserve(scene.outputs.size() * 2 + 1);

    channels_.clear();
    channels_.reserve(scene.channels.size());
    bool anySolo = false;
    for (std::size_t i = 0; i < scene.channels.size(); ++i) {
        const Channel& channel = scene.channels[i];
        channelIndex.emplace(channel.id, i);
        const float theta = (channel.pan + 1.0f) * kPi / 4.0f;
        ChannelCache cache;
        cache.gain = dbToLinear(channel.gainDb);
        cache.panLeft = std::cos(theta);
        cache.panRight = std::sin(theta);
        cache.muted = channel.muted;
        cache.soloed = channel.soloed;
        channels_.push_back(cache);
        anySolo = anySolo || channel.soloed;
    }
    for (std::size_t i = 0; i < scene.outputs.size(); ++i) {
        outputIndex.emplace(scene.outputs[i].id, i);
    }

    routes_.clear();
    routes_.reserve(scene.routes.size());
    for (const Route& route : scene.routes) {
        RouteCache cache;
        cache.channelIndex = channelIndex[route.channelId];
        cache.outputIndex = outputIndex[route.outputId];
        cache.sendGain = dbToLinear(route.sendGainDb);
        routes_.push_back(cache);
    }

    const int maxFrames = maxBlockFrames < 0 ? 0 : maxBlockFrames;
    maxBlockFrames_ = maxFrames;
    outputCount_ = scene.outputs.size();
    anySolo_ = anySolo;

    const std::size_t stereoFrames = static_cast<std::size_t>(maxFrames) * 2;
    scratch_.assign(channels_.size() * stereoFrames, 0.0f);
    accumulators_.assign(outputCount_ * stereoFrames, 0.0f);
    return true;
}

void MixerEngine::render(std::span<const AudioBlock> channelInputs,
                         std::span<AudioBlock> outputBlocks) noexcept {
    const std::size_t numChannels = std::min(channelInputs.size(), channels_.size());
    const std::size_t numOutputs = std::min(outputBlocks.size(), outputCount_);
    const int maxFrames = maxBlockFrames_;
    if (maxFrames <= 0 || numChannels == 0 || numOutputs == 0) {
        return;
    }

    for (std::size_t o = 0; o < numOutputs; ++o) {
        const int frames = clampFrames(outputBlocks[o].numFrames, maxFrames);
        float* accLeft = accumulators_.data() + (o * 2) * static_cast<std::size_t>(maxFrames);
        float* accRight = accLeft + maxFrames;
        for (int i = 0; i < frames; ++i) {
            accLeft[i] = 0.0f;
            accRight[i] = 0.0f;
        }
    }

    for (std::size_t c = 0; c < numChannels; ++c) {
        const AudioBlock& input = channelInputs[c];
        const ChannelCache& cache = channels_[c];
        const int frames = clampFrames(input.numFrames, maxFrames);
        float* scratchLeft = scratch_.data() + (c * 2) * static_cast<std::size_t>(maxFrames);
        float* scratchRight = scratchLeft + maxFrames;
        const bool silenced = cache.muted || (anySolo_ && !cache.soloed);
        if (silenced || cache.gain == 0.0f || frames == 0 || input.channels == nullptr ||
            input.channels[0] == nullptr || (input.numChannels > 1 && input.channels[1] == nullptr)) {
            for (int i = 0; i < frames; ++i) {
                scratchLeft[i] = 0.0f;
                scratchRight[i] = 0.0f;
            }
        } else if (input.numChannels <= 1) {
            // Mono source: upmix by copying the single input channel into both L and R.
            const float* mono = input.channels[0];
            const float leftScale = cache.gain * cache.panLeft;
            const float rightScale = cache.gain * cache.panRight;
            for (int i = 0; i < frames; ++i) {
                scratchLeft[i] = mono[i] * leftScale;
                scratchRight[i] = mono[i] * rightScale;
            }
        } else {
            const float* inLeft = input.channels[0];
            const float* inRight = input.channels[1];
            const float leftScale = cache.gain * cache.panLeft;
            const float rightScale = cache.gain * cache.panRight;
            for (int i = 0; i < frames; ++i) {
                scratchLeft[i] = inLeft[i] * leftScale;
                scratchRight[i] = inRight[i] * rightScale;
            }
        }
    }

    for (const RouteCache& route : routes_) {
        if (route.channelIndex >= numChannels || route.outputIndex >= numOutputs) {
            continue;
        }
        if (route.sendGain == 0.0f) {
            continue;
        }
        const int channelFrames =
            clampFrames(channelInputs[route.channelIndex].numFrames, maxFrames);
        const int outputFrames =
            clampFrames(outputBlocks[route.outputIndex].numFrames, maxFrames);
        const int frames = channelFrames < outputFrames ? channelFrames : outputFrames;
        const float* scratchLeft =
            scratch_.data() + (route.channelIndex * 2) * static_cast<std::size_t>(maxFrames);
        const float* scratchRight = scratchLeft + maxFrames;
        float* accLeft =
            accumulators_.data() + (route.outputIndex * 2) * static_cast<std::size_t>(maxFrames);
        float* accRight = accLeft + maxFrames;
        for (int i = 0; i < frames; ++i) {
            accLeft[i] += scratchLeft[i] * route.sendGain;
            accRight[i] += scratchRight[i] * route.sendGain;
        }
    }

    for (std::size_t o = 0; o < numOutputs; ++o) {
        AudioBlock& output = outputBlocks[o];
        const int frames = clampFrames(output.numFrames, maxFrames);
        if (output.channels == nullptr) {
            continue;
        }
        const float* accLeft =
            accumulators_.data() + (o * 2) * static_cast<std::size_t>(maxFrames);
        const float* accRight = accLeft + maxFrames;
        if (output.numChannels >= 1 && output.channels[0] != nullptr) {
            for (int i = 0; i < frames; ++i) {
                output.channels[0][i] = accLeft[i];
            }
        }
        if (output.numChannels >= 2 && output.channels[1] != nullptr) {
            for (int i = 0; i < frames; ++i) {
                output.channels[1][i] = accRight[i];
            }
        }
    }
}

int MixerEngine::channelCount() const noexcept { return static_cast<int>(channels_.size()); }

int MixerEngine::outputCount() const noexcept { return static_cast<int>(outputCount_); }

} // namespace pluginrack
