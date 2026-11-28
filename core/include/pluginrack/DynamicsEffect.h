#pragma once

#include <cstdint>
#include <vector>

#include "pluginrack/DspMath.h"

namespace pluginrack {

enum class DynamicsEffectType { Compressor, Gate };

// Editor and validator ranges for the gate/compressor depth controls. One
// home so the window, the validator and the tests cannot drift apart.
inline constexpr float kDynamicsMinHoldMs = 0.0f;
inline constexpr float kDynamicsMaxHoldMs = 2000.0f;
inline constexpr float kDynamicsMinHysteresisDb = 0.0f;
inline constexpr float kDynamicsMaxHysteresisDb = 24.0f;
inline constexpr float kDynamicsMinSidechainHz = 0.0f;
inline constexpr float kDynamicsMaxSidechainHz = 1000.0f;
inline constexpr float kDynamicsMinMixPercent = 0.0f;
inline constexpr float kDynamicsMaxMixPercent = 100.0f;

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
    // Gate depth (schema 14). holdMs keeps the gate open after the detector
    // falls below the open threshold, hysteresisDb lowers the level at which
    // it closes again, and sidechainHz high-passes the detector only
    // (0 disables the filter). All defaults reproduce the pre-v14 gate.
    float holdMs = 0.0f;
    float hysteresisDb = 0.0f;
    float sidechainHz = 0.0f;
    // Compressor detector and blend (schema 14). rmsDetector false keeps the
    // linked peak detector; mixPercent 100 is the fully processed signal and
    // 0 the untouched dry input.
    bool rmsDetector = false;
    float mixPercent = 100.0f;

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
    // Detector power for the RMS mode; unused (and untouched) by the peak path.
    float envelopeSquared_ = 0.0f;
    float gain_ = 1.0f;
    // Gate hold/hysteresis state. Both stay inert unless the effect asks for
    // them, so a default gate runs the pre-v14 gain law unchanged.
    bool gateOpen_ = false;
    int gateHoldRemaining_ = 0;
    // Detector-only high-pass for sidechainHz (audio is never filtered).
    dsp::Biquad sidechainCoeffs_;
    float sidechainL1_ = 0.0f;
    float sidechainL2_ = 0.0f;
    float sidechainR1_ = 0.0f;
    float sidechainR2_ = 0.0f;
};

} // namespace pluginrack
