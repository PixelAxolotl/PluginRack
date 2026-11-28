#include "pluginrack/DynamicsEffect.h"

#include <algorithm>
#include <cmath>

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
    envelopeSquared_ = 0.0f;
    gain_ = 1.0f;
    gateOpen_ = false;
    gateHoldRemaining_ = 0;
    sidechainL1_ = 0.0f;
    sidechainL2_ = 0.0f;
    sidechainR1_ = 0.0f;
    sidechainR2_ = 0.0f;
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
    // Gain-domain knee edges, hoisted out of the sample loop so the branches
    // can compare envelopes directly. Only the soft-knee blend still needs
    // dB; below/above the knee the same curve is a plain gain or one pow.
    const float gateRange = std::max(effect_.ratio, 0.0f);
    const float knee = std::max(effect_.kneeDb, 0.0f);
    const float halfKnee = knee * 0.5f;
    const float thresholdDb = effect_.thresholdDb;
    const float thresholdGain = dsp::dbToGain(thresholdDb);
    const float lowKneeGain = dsp::dbToGain(thresholdDb - halfKnee);
    const float highKneeGain = dsp::dbToGain(thresholdDb + halfKnee);
    const float rangeGain = dsp::dbToGain(-gateRange);
    const float compSlope = 1.0f / ratio - 1.0f;
    // v14 gate depth. hold/hysteresis only engage when the user moves them
    // off their defaults, which keeps a stock gate bit-identical to pre-v14.
    const bool gateDepth = gate && (effect_.holdMs > kDynamicsMinHoldMs ||
                                    effect_.hysteresisDb > kDynamicsMinHysteresisDb);
    const int holdSamples = gateDepth
        ? static_cast<int>(std::lround(std::clamp(effect_.holdMs, kDynamicsMinHoldMs,
                                                  kDynamicsMaxHoldMs) *
                                       0.001 * sampleRate_))
        : 0;
    // Hysteresis is a one-sided threshold shift: the gate opens at threshold
    // and closes only below threshold - hysteresis.
    const float closeGain = dsp::dbToGain(
        thresholdDb - std::clamp(effect_.hysteresisDb, kDynamicsMinHysteresisDb,
                                 kDynamicsMaxHysteresisDb));
    const bool sidechain = effect_.sidechainHz > kDynamicsMinSidechainHz;
    if (sidechain) {
        sidechainCoeffs_ = dsp::highPass(
            std::clamp(effect_.sidechainHz, kDynamicsMinSidechainHz,
                       kDynamicsMaxSidechainHz),
            sampleRate_);
    }
    // RMS mode averages power over a fixed 10 ms window: an RMS detector's
    // time constant is the measurement window, not the gain attack/release,
    // which still shape the gain below. A one-pole on power is the cheap but
    // standard estimator for this.
    const float rmsAveraging = dsp::smoothingCoeff(sampleRate_, 10.0);
    // Dry/wet blend. A non-finite setting falls back to fully processed so it
    // cannot leak an unsanitised dry sample past the input scrub.
    const float mix = std::isfinite(effect_.mixPercent)
        ? std::clamp(effect_.mixPercent, kDynamicsMinMixPercent, kDynamicsMaxMixPercent) *
              0.01f
        : 1.0f;

    for (int i = 0; i < frames; ++i) {
        // A single non-finite sample (or non-finite trim) must not poison the
        // envelope for the rest of the session: silence it at the door. The
        // dry copy stays exactly as it arrived for the mix stage.
        const float dryL = left[i];
        const float dryR = right[i];
        float sL = dryL * inputGain;
        float sR = dryR * inputGain;
        if (!std::isfinite(sL)) sL = 0.0f;
        if (!std::isfinite(sR)) sR = 0.0f;
        float level = 0.0f;
        if (sidechain) {
            // The high-pass shapes the detector only: sL/sR themselves are
            // left alone, so the audio path is untouched.
            const float filteredL = dsp::applyBiquadSample(sL, sidechainCoeffs_,
                                                           sidechainL1_, sidechainL2_);
            const float filteredR = dsp::applyBiquadSample(sR, sidechainCoeffs_,
                                                           sidechainR1_, sidechainR2_);
            level = std::max(std::fabs(filteredL), std::fabs(filteredR));
        } else {
            level = std::max(std::fabs(sL), std::fabs(sR));
        }
        if (effect_.rmsDetector) {
            const float power = level * level;
            envelopeSquared_ += (power - envelopeSquared_) * rmsAveraging;
            dsp::flushState(envelopeSquared_);
            envelope_ = std::sqrt(std::max(envelopeSquared_, 0.0f));
        } else {
            envelope_ += (level - envelope_) * (level > envelope_ ? attack : release);
            dsp::flushState(envelope_);
        }
        // staticGain is the curve's gain without makeup; the dB form below is
        // the only path that still converts, and only inside the knee.
        float staticGain = 1.0f;
        if (gate) {
            // Hold pins the open state for holdMs after the detector falls
            // below the open threshold; hysteresis widens the band the gate
            // has to fall through before it closes.
            if (gateDepth) {
                if (level >= thresholdGain) {
                    gateOpen_ = true;
                    gateHoldRemaining_ = holdSamples;
                } else if (gateOpen_) {
                    if (gateHoldRemaining_ > 0) {
                        --gateHoldRemaining_;
                    } else if (level < closeGain) {
                        gateOpen_ = false;
                    }
                }
            }
            if (!gateDepth || (!gateOpen_ && gateHoldRemaining_ <= 0)) {
                // Downward expander: below the threshold the output drops by Range
                // dB (stored in ratio), and the knee spreads that corner.
                if (knee > 0.0f && envelope_ > lowKneeGain && envelope_ < highKneeGain) {
                    const float distance = dsp::gainToDb(envelope_) - thresholdDb;
                    staticGain = dsp::dbToGain(-gateRange * (halfKnee - distance) / knee);
                } else if (envelope_ <= lowKneeGain) {
                    staticGain = rangeGain;
                }
            }
        } else if (knee > 0.0f && envelope_ > lowKneeGain && envelope_ < highKneeGain) {
            const float distance = dsp::gainToDb(envelope_) - thresholdDb;
            const float compressed = distance + halfKnee;
            staticGain = dsp::dbToGain(compSlope * compressed * compressed / (2.0f * knee));
        } else if (envelope_ > thresholdGain) {
            // Above the knee: dB gain is compSlope * distance, i.e.
            // 10^(compSlope*distance/20) = (envelope/threshold)^compSlope.
            staticGain = std::pow(envelope_ / thresholdGain, compSlope);
        }
        const float targetGain = staticGain * makeup;
        const float gainRate = targetGain < gain_ ? attack : release;
        gain_ += (targetGain - gain_) * gainRate;
        dsp::flushState(gain_);
        const float wetL = sL * gain_;
        const float wetR = sR * gain_;
        // The dry copy is the input as it arrived, with the same non-finite
        // scrub as the wet path so mix 0 cannot leak a NaN downstream.
        const float baseL = std::isfinite(dryL) ? dryL : 0.0f;
        const float baseR = std::isfinite(dryR) ? dryR : 0.0f;
        if (mix >= 1.0f) {
            left[i] = wetL;
            right[i] = wetR;
        } else if (mix > 0.0f) {
            left[i] = baseL + (wetL - baseL) * mix;
            right[i] = baseR + (wetR - baseR) * mix;
        } else {
            left[i] = baseL;
            right[i] = baseR;
        }
    }
}
} // namespace pluginrack
