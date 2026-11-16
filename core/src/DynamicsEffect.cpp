#include "pluginrack/DynamicsEffect.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "pluginrack/DspMath.h"

namespace pluginrack {

std::vector<DynamicsEffect> defaultDynamicsEffects() {
    return {
        // Session default compressor: -12 dB threshold, 8:1, 40 ms attack,
        // 300 ms release, 8 dB knee, unity trim and output.
        DynamicsEffect{1, DynamicsEffectType::Compressor, true, -12.0f, 8.0f, 40.0f, 300.0f,
                       0.0f, 0.0f, 8.0f},
        // Range is the depth below threshold: 30 dB is an audible gate that
        // still lets a quiet tail through. 5 dB was inaudible on real material,
        // which made the gate look broken when it was only too gentle.
        DynamicsEffect{2, DynamicsEffectType::Gate, true, -48.0f, 30.0f, 1.0f, 100.0f}
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
    const float ratio = std::max(effect_.ratio, 1.0f);
    const float attack = dsp::smoothingCoeff(sampleRate_, effect_.attackMs);
    // Anything faster than 10 ms release zippers on musical material.
    const float release = dsp::smoothingCoeff(sampleRate_, std::max(effect_.releaseMs, 10.0f));
    const float inputGain = dsp::dbToGain(effect_.inputGainDb);
    const float makeup = dsp::dbToGain(effect_.makeupDb);

    for (int i = 0; i < frames; ++i) {
        left[i] *= inputGain;
        right[i] *= inputGain;
        const float level = std::max(std::fabs(left[i]), std::fabs(right[i]));
        envelope_ += (level - envelope_) * (level > envelope_ ? attack : release);
        const float levelDb = dsp::gainToDb(envelope_);
        float targetDb = 0.0f;
        if (gate) {
            // Downward expander: below the threshold the output drops by Range
            // dB (stored in ratio), and the knee spreads that corner.
            const float range = std::max(effect_.ratio, 0.0f);
            const float knee = std::max(effect_.kneeDb, 0.0f);
            const float distance = levelDb - effect_.thresholdDb;
            if (knee > 0.0f && distance < knee * 0.5f && distance > -knee * 0.5f) {
                targetDb = -range * (knee * 0.5f - distance) / knee;
            } else if (distance <= -knee * 0.5f) {
                targetDb = -range;
            }
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
        const float targetGain = dsp::dbToGain(targetDb) * makeup;
        const float gainRate = targetGain < gain_ ? attack : release;
        gain_ += (targetGain - gain_) * gainRate;
        left[i] *= gain_;
        right[i] *= gain_;
    }
}
} // namespace pluginrack
