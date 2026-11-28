#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "DeEsserEffect.h"
#include "MultibandEffect.h"

namespace pluginrack {

// Every trace below is the engine's own response: the multiband curves
// evaluate the LR4 split coefficients the processor runs, the de-esser curve
// its band-pass and gain law. `sampleRateHz` must match the audio device.
std::vector<float> makeMultibandTargetTrace(const MultibandEffect& effect,
                                            std::size_t binCount,
                                            float minHz = 20.0f,
                                            float maxHz = 20000.0f,
                                            double sampleRateHz = 48000.0);

std::vector<float> makeMultibandGainChangeTrace(const MultibandEffect& effect,
                                                const std::vector<float>& reductionsDb,
                                                std::size_t binCount,
                                                float minHz = 20.0f,
                                                float maxHz = 20000.0f,
                                                double sampleRateHz = 48000.0);

std::vector<float> makeDeEsserGainReductionTrace(const DeEsserEffect& effect,
                                                 float reductionDb,
                                                 std::size_t binCount,
                                                 float minHz = 20.0f,
                                                 float maxHz = 20000.0f,
                                                 double sampleRateHz = 48000.0);

// In-place forms, for callers that rebuild a trace every UI tick: they resize
// `out` to binCount and reuse its existing capacity, so a steady-size trace
// stops allocating. The returning make* helpers are thin wrappers over these.
void fillMultibandGainChangeTrace(const MultibandEffect& effect,
                                  const std::vector<float>& reductionsDb,
                                  std::size_t binCount,
                                  std::vector<float>& out,
                                  float minHz = 20.0f,
                                  float maxHz = 20000.0f,
                                  double sampleRateHz = 48000.0);

void fillDeEsserGainReductionTrace(const DeEsserEffect& effect,
                                   float reductionDb,
                                   std::size_t binCount,
                                   std::vector<float>& out,
                                   float minHz = 20.0f,
                                   float maxHz = 20000.0f,
                                   double sampleRateHz = 48000.0);

class LevelHistory final {
public:
    explicit LevelHistory(std::size_t capacity);

    void push(float inputDb, float reductionDb, float outputDb) noexcept;
    std::size_t size() const noexcept { return size_; }
    void copyOrderedTo(std::span<float> inputDb,
                       std::span<float> reductionDb,
                       std::span<float> outputDb) const noexcept;

private:
    std::vector<float> inputDb_;
    std::vector<float> reductionDb_;
    std::vector<float> outputDb_;
    std::size_t next_ = 0;
    std::size_t size_ = 0;
};

} // namespace pluginrack
