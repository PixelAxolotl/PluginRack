#include "pluginrack/OfflineRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "pluginrack/AudioBlock.h"
#include "pluginrack/MixerEngine.h"

namespace pluginrack {

namespace {

constexpr int kBlockFrames = 256;
constexpr double kTwoPi = 6.28318530717958647692;

} // namespace

std::vector<std::vector<float>> renderSine(const Scene& scene,
                                           const SceneAvailability& availability,
                                           float toneHz, float amplitude,
                                           int numFrames) {
    MixerEngine engine;
    std::string error;
    if (!engine.prepare(scene, availability, kBlockFrames, &error)) {
        throw std::runtime_error("renderSine: " + error);
    }
    const std::size_t numChannels = scene.channels.size();
    const std::size_t numOutputs = scene.outputs.size();
    std::vector<std::vector<float>> result(numOutputs);
    if (numFrames <= 0) {
        return result;
    }
    for (auto& buffer : result) {
        buffer.reserve(static_cast<std::size_t>(numFrames) * 2);
    }

    const double sampleRate = scene.sampleRate > 0.0 ? scene.sampleRate : 48000.0;
    const double phaseStep = static_cast<double>(toneHz) * kTwoPi / sampleRate;

    std::vector<std::vector<float>> inLeft(numChannels, std::vector<float>(kBlockFrames));
    std::vector<std::vector<float>> inRight(numChannels, std::vector<float>(kBlockFrames));
    std::vector<std::vector<float>> outLeft(numOutputs, std::vector<float>(kBlockFrames));
    std::vector<std::vector<float>> outRight(numOutputs, std::vector<float>(kBlockFrames));
    std::vector<std::array<float*, 2>> inPtrs(numChannels);
    std::vector<std::array<float*, 2>> outPtrs(numOutputs);
    std::vector<AudioBlock> inputBlocks(numChannels);
    std::vector<AudioBlock> outputBlocks(numOutputs);
    for (std::size_t c = 0; c < numChannels; ++c) {
        inPtrs[c] = {inLeft[c].data(), inRight[c].data()};
    }
    for (std::size_t o = 0; o < numOutputs; ++o) {
        outPtrs[o] = {outLeft[o].data(), outRight[o].data()};
    }

    int rendered = 0;
    while (rendered < numFrames) {
        const int blockFrames = std::min(kBlockFrames, numFrames - rendered);
        for (std::size_t c = 0; c < numChannels; ++c) {
            for (int i = 0; i < blockFrames; ++i) {
                const double phase =
                    phaseStep * static_cast<double>(rendered + i);
                const float sine = amplitude * static_cast<float>(std::sin(phase));
                inLeft[c][static_cast<std::size_t>(i)] = sine;
                inRight[c][static_cast<std::size_t>(i)] = sine;
            }
            inputBlocks[c] = AudioBlock{inPtrs[c].data(), 2, blockFrames};
        }
        for (std::size_t o = 0; o < numOutputs; ++o) {
            outputBlocks[o] = AudioBlock{outPtrs[o].data(), 2, blockFrames};
        }
        engine.render(inputBlocks, outputBlocks);
        for (std::size_t o = 0; o < numOutputs; ++o) {
            for (int i = 0; i < blockFrames; ++i) {
                result[o].push_back(outLeft[o][static_cast<std::size_t>(i)]);
                result[o].push_back(outRight[o][static_cast<std::size_t>(i)]);
            }
        }
        rendered += blockFrames;
    }
    return result;
}

} // namespace pluginrack
