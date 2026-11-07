#pragma once

#include <cstdint>
#include <vector>

namespace pluginrack {

struct LimiterEffect {
    std::uint32_t id = 0;
    bool bypassed = false;
    float inputGainDb = 0.0f;
    float ceilingDb = -0.1f;
    float lookaheadMs = 1.5f;
    float releaseMs = 100.0f;
    bool operator==(const LimiterEffect&) const = default;
};

LimiterEffect makeDefaultLimiterEffect(std::uint32_t id = 1);
std::vector<LimiterEffect> defaultLimiterEffects();
float limiterStaticGainDb(float levelDb, const LimiterEffect& effect);

class LimiterProcessor {
public:
    void prepare(double sampleRate);
    void setEffect(const LimiterEffect& effect);
    void process(float* left, float* right, int frames) noexcept;
    void reset() noexcept;
    float gainReductionDb() const noexcept { return reductionDb_; }
    float outputPeakDb() const noexcept { return peakDb_; }

private:
    double sampleRate_ = 48000.0;
    LimiterEffect effect_;
    std::vector<float> delayL_;
    std::vector<float> delayR_;
    int delayCapacity_ = 0;
    int delayWrite_ = 0;
    float gain_ = 1.0f;
    float reductionDb_ = 0.0f;
    float peakDb_ = -60.0f;
    float peakEnv_ = 0.0f;
    void ensureDelay();
    int lookaheadSamples() const noexcept;
};

} // namespace pluginrack
