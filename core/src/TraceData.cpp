#include "pluginrack/TraceData.h"

#include <algorithm>
#include <cmath>

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
                                            float maxHz,
                                            double sampleRateHz) {
    std::vector<float> trace(binCount, 0.0f);
    if (binCount == 0 || effect.bands.empty()) return trace;

    for (std::size_t bin = 0; bin < binCount; ++bin) {
        const float position = binCount > 1
            ? static_cast<float>(bin) / static_cast<float>(binCount - 1)
            : 0.5f;
        const float frequency = logFrequency(position, minHz, maxHz);
        const auto bandIndex = bandForFrequency(effect, frequency);
        const auto& band = effect.bands[bandIndex];
        // The band's real LR4 split magnitude at this bin plus its trims. The
        // static curve is evaluated at the band's threshold, where it is
        // exactly 0 dB, so the target is the shape the audio path produces
        // rather than a per-band level.
        const float magnitude = multibandBandMagnitude(effect, bandIndex, frequency, sampleRateHz);
        const float gainDb = dsp::gainToDb(std::max(magnitude, 1.0e-6f))
            + finiteOr(band.inputGainDb, 0.0f)
            + finiteOr(band.makeupDb, 0.0f)
            + multibandStaticGainDb(finiteOr(band.thresholdDb, -18.0f), band);
        trace[bin] = clampGraphDb(gainDb);
    }
    return trace;
}

void fillMultibandGainChangeTrace(const MultibandEffect& effect,
                                  const std::vector<float>& reductionsDb,
                                  std::size_t binCount,
                                  std::vector<float>& out,
                                  float minHz,
                                  float maxHz,
                                  double sampleRateHz) {
    out.assign(binCount, 0.0f);
    if (binCount == 0 || effect.bands.empty()) return;

    for (std::size_t bin = 0; bin < binCount; ++bin) {
        const float position = binCount > 1
            ? static_cast<float>(bin) / static_cast<float>(binCount - 1)
            : 0.5f;
        const float frequency = logFrequency(position, minHz, maxHz);
        const auto bandIndex = bandForFrequency(effect, frequency);
        const float reduction = bandIndex < reductionsDb.size()
            ? std::abs(finiteOr(reductionsDb[bandIndex], 0.0f))
            : 0.0f;
        // The band only reduces where it has signal: weight the meter value by
        // the split's power response at this bin.
        const float magnitude = multibandBandMagnitude(effect, bandIndex, frequency, sampleRateHz);
        out[bin] = clampGraphDb(-reduction * magnitude * magnitude);
    }
}

std::vector<float> makeMultibandGainChangeTrace(const MultibandEffect& effect,
                                                const std::vector<float>& reductionsDb,
                                                std::size_t binCount,
                                                float minHz,
                                                float maxHz,
                                                double sampleRateHz) {
    std::vector<float> trace;
    fillMultibandGainChangeTrace(effect, reductionsDb, binCount, trace, minHz, maxHz, sampleRateHz);
    return trace;
}

void fillDeEsserGainReductionTrace(const DeEsserEffect& effect,
                                   float reductionDb,
                                   std::size_t binCount,
                                   std::vector<float>& out,
                                   float minHz,
                                   float maxHz,
                                   double sampleRateHz) {
    out.assign(binCount, 0.0f);
    const float reduction = std::abs(finiteOr(reductionDb, 0.0f));
    const auto band = deEsserBandCoefficients(effect, sampleRateHz);
    // Split mode leaves everything outside the band alone and scales the band
    // by g: y = in + band*(g - 1), so the steady-state gain at a bin is
    // 1 + (g - 1)*H, H the band-pass response. Wideband mode scales the whole
    // signal, i.e. flat.
    const bool split = effect.mode == DeEsserMode::Split;
    const float gain = std::pow(10.0f, -reduction / 20.0f);
    for (std::size_t bin = 0; bin < binCount; ++bin) {
        const float position = binCount > 1
            ? static_cast<float>(bin) / static_cast<float>(binCount - 1)
            : 0.5f;
        const float frequency = logFrequency(position, minHz, maxHz);
        float binDb = -reduction;
        if (split) {
            const auto bandResponse = dsp::biquadResponse(band, frequency, sampleRateHz);
            const float re = 1.0f + (gain - 1.0f) * bandResponse.re;
            const float im = (gain - 1.0f) * bandResponse.im;
            binDb = dsp::gainToDb(std::sqrt(re * re + im * im));
        }
        out[bin] = clampGraphDb(binDb);
    }
}

std::vector<float> makeDeEsserGainReductionTrace(const DeEsserEffect& effect,
                                                 float reductionDb,
                                                 std::size_t binCount,
                                                 float minHz,
                                                 float maxHz,
                                                 double sampleRateHz) {
    std::vector<float> trace;
    fillDeEsserGainReductionTrace(effect, reductionDb, binCount, trace, minHz, maxHz, sampleRateHz);
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
