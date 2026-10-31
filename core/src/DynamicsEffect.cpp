#include "pluginrack/DynamicsEffect.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pluginrack {
namespace {
float dbToGain(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
float gainToDb(float gain) noexcept {
    return 20.0f * std::log10(std::max(gain, 1.0e-8f));
}
float smoothing(double rate, float ms) noexcept {
    return 1.0f - std::exp(-1.0f / (static_cast<float>(rate) * std::max(ms, 0.1f) * 0.001f));
}
}

std::vector<DynamicsEffect> defaultDynamicsEffects() {
    return {
        DynamicsEffect{1, DynamicsEffectType::Compressor, true},
        DynamicsEffect{2, DynamicsEffectType::Limiter, true, -1.0f, 20.0f, 0.1f, 50.0f},
        DynamicsEffect{3, DynamicsEffectType::Gate, true, -40.0f, 10.0f, 1.0f, 100.0f}
    };
}

void DynamicsProcessor::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void DynamicsProcessor::setEffect(const DynamicsEffect& effect) {
    effect_ = effect;
}

void DynamicsProcessor::reset() noexcept {
    envelope_ = 0.0f;
    gain_ = 1.0f;
}

void DynamicsProcessor::process(float* left, float* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0 || effect_.bypassed) return;
    const bool gate = effect_.type == DynamicsEffectType::Gate;
    const bool limiter = effect_.type == DynamicsEffectType::Limiter;
    const float ratio = limiter ? 20.0f : std::max(effect_.ratio, 1.0f);
    const float attack = smoothing(sampleRate_, effect_.attackMs);
    const float release = smoothing(sampleRate_, effect_.releaseMs);
    const float inputGain = dbToGain(effect_.inputGainDb);
    const float makeup = dbToGain(effect_.makeupDb);

    for (int i = 0; i < frames; ++i) {
        left[i] *= inputGain;
        right[i] *= inputGain;
        const float level = std::max(std::fabs(left[i]), std::fabs(right[i]));
        envelope_ += (level - envelope_) * (level > envelope_ ? attack : release);
        const float levelDb = gainToDb(envelope_);
        float targetDb = 0.0f;
        if (gate) {
            if (levelDb < effect_.thresholdDb) targetDb = -80.0f;
        } else {
            const float knee = std::max(effect_.kneeDb, 0.0f);
            const float distance = levelDb - effect_.thresholdDb;
            if (knee > 0.0f && distance > -knee * 0.5f && distance < knee * 0.5f) {
                const float compressed = distance + knee * 0.5f;
                targetDb = (1.0f / ratio - 1.0f) * compressed * compressed / (2.0f * knee);
            } else if (distance > 0.0f) {
                targetDb = effect_.thresholdDb + distance / ratio - levelDb;
            }
        }
        const float targetGain = dbToGain(targetDb) * makeup;
        const float gainRate = targetGain < gain_ ? attack : release;
        gain_ += (targetGain - gain_) * gainRate;
        left[i] *= gain_;
        right[i] *= gain_;
    }
}
} // namespace pluginrack
