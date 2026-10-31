#pragma once

#include <cstdint>
#include <vector>

namespace pluginrack {

enum class DynamicsEffectType { Compressor, Limiter, Gate };

struct DynamicsEffect {
    std::uint32_t id = 0;
    DynamicsEffectType type = DynamicsEffectType::Compressor;
    bool bypassed = false;
    float thresholdDb = -18.0f;
    float ratio = 4.0f;
    float attackMs = 10.0f;
    float releaseMs = 100.0f;
    float makeupDb = 0.0f;
    float inputGainDb = 0.0f;
    float kneeDb = 0.0f;

    bool operator==(const DynamicsEffect&) const = default;
};

std::vector<DynamicsEffect> defaultDynamicsEffects();

class DynamicsProcessor {
public:
    void prepare(double sampleRate);
    void setEffect(const DynamicsEffect& effect);
    void process(float* left, float* right, int frames) noexcept;
    void reset() noexcept;

private:
    double sampleRate_ = 48000.0;
    DynamicsEffect effect_;
    float envelope_ = 0.0f;
    float gain_ = 1.0f;
};

} // namespace pluginrack
