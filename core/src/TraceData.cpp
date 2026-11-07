#include "pluginrack/TraceData.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pluginrack {
namespace {

constexpr float kGraphFloorDb = -60.0f;
constexpr float kGraphCeilingDb = 12.0f;

float finiteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

float clampGraphDb(float value) {
    return std::clamp(finiteOr(value, kGraphFloorDb), kGraphFloorDb, kGraphCeilingDb);
}

float logFrequency(float position, float minHz, float maxHz) {
    const auto safeMin = std::max(1.0e-3f, finiteOr(minHz, 20.0f));
    const auto safeMax = std::max(safeMin, finiteOr(maxHz, 20000.0f));
    return safeMin * std::pow(safeMax / safeMin, std::clamp(position, 0.0f, 1.0f));
}

std::size_t bandForFrequency(const MultibandEffect& effect, float frequency) {
    for (std::size_t i = 0; i < effect.crossovers.size(); ++i) {
        if (frequency < finiteOr(effect.crossovers[i], 20000.0f))
            return std::min(i, effect.bands.empty() ? std::size_t{0} : effect.bands.size() - 1);
    }
    return effect.bands.empty() ? 0 : effect.bands.size() - 1;
}

} // namespace

std::vector<float> makeMultibandTargetTrace(const MultibandEffect& effect,
                                            std::size_t binCount,
                                            float minHz,
                                            float maxHz) {
    std::vector<float> trace(binCount, 0.0f);
    if (binCount == 0 || effect.bands.empty()) return trace;

    for (std::size_t bin = 0; bin < binCount; ++bin) {
        const float position = binCount > 1
            ? static_cast<float>(bin) / static_cast<float>(binCount - 1)
            : 0.5f;
        const float frequency = logFrequency(position, minHz, maxHz);
        const auto bandIndex = bandForFrequency(effect, frequency);
        const auto& band = effect.bands[bandIndex];
        const float amplitude = std::clamp(
            0.8f + std::abs(finiteOr(band.makeupDb, 0.0f)) * 0.15f
                + std::max(0.0f, finiteOr(band.ratio, 1.0f) - 1.0f) * 0.35f
                + std::max(0.0f, finiteOr(band.kneeDb, 0.0f)) * 0.04f,
            0.5f, 8.0f);
        trace[bin] = clampGraphDb(amplitude);
    }
    return trace;
}

std::vector<float> makeMultibandGainChangeTrace(const MultibandEffect& effect,
                                                const std::vector<float>& reductionsDb,
                                                std::size_t binCount,
                                                float minHz,
                                                float maxHz) {
    std::vector<float> trace(binCount, 0.0f);
    if (binCount == 0 || effect.bands.empty()) return trace;

    for (std::size_t bin = 0; bin < binCount; ++bin) {
        const float position = binCount > 1
            ? static_cast<float>(bin) / static_cast<float>(binCount - 1)
            : 0.5f;
        const float frequency = logFrequency(position, minHz, maxHz);
        const auto bandIndex = bandForFrequency(effect, frequency);
        const float reduction = bandIndex < reductionsDb.size()
            ? std::abs(finiteOr(reductionsDb[bandIndex], 0.0f))
            : 0.0f;
        trace[bin] = clampGraphDb(-reduction);
    }
    return trace;
}

std::vector<float> makeDeEsserGainReductionTrace(const DeEsserEffect& effect,
                                                 float reductionDb,
                                                 std::size_t binCount,
                                                 float minHz,
                                                 float maxHz) {
    std::vector<float> trace(binCount, 0.0f);
    const float reduction = std::abs(finiteOr(reductionDb, 0.0f));
    const float center = std::clamp(finiteOr(effect.freqHz, 6000.0f), minHz, maxHz);
    const float q = std::max(0.1f, finiteOr(effect.q, 2.0f));
    for (std::size_t bin = 0; bin < binCount; ++bin) {
        const float position = binCount > 1
            ? static_cast<float>(bin) / static_cast<float>(binCount - 1)
            : 0.5f;
        const float frequency = logFrequency(position, minHz, maxHz);
        const float distance = std::log2(std::max(1.0f, frequency) / center);
        const float width = 0.85f / q;
        trace[bin] = clampGraphDb(-reduction * std::exp(-0.5f * (distance / width) * (distance / width)));
    }
    return trace;
}

LevelHistory::LevelHistory(std::size_t capacity)
    : inputDb_(capacity, kGraphFloorDb),
      reductionDb_(capacity, 0.0f),
      outputDb_(capacity, kGraphFloorDb) {}

void LevelHistory::push(float inputDb, float reductionDb, float outputDb) noexcept {
    if (inputDb_.empty()) return;
    inputDb_[next_] = clampGraphDb(inputDb);
    reductionDb_[next_] = clampGraphDb(-std::abs(finiteOr(reductionDb, 0.0f)));
    outputDb_[next_] = clampGraphDb(outputDb);
    next_ = (next_ + 1) % inputDb_.size();
    size_ = std::min(size_ + 1, inputDb_.size());
}

void LevelHistory::copyOrderedTo(std::span<float> inputDb,
                                 std::span<float> reductionDb,
                                 std::span<float> outputDb) const noexcept {
    const auto count = std::min({size_, inputDb.size(), reductionDb.size(), outputDb.size()});
    const auto start = size_ == inputDb_.size() ? next_ : 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto source = (start + i) % inputDb_.size();
        inputDb[i] = inputDb_[source];
        reductionDb[i] = reductionDb_[source];
        outputDb[i] = outputDb_[source];
    }
}

} // namespace pluginrack
