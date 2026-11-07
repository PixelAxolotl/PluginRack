#pragma once

#include <cstdint>
#include <vector>

namespace pluginrack {

enum class DeEsserMode { Split = 0, Wideband = 1 };

struct DeEsserEffect {
    std::uint32_t id = 0;
    bool bypassed = false;
    DeEsserMode mode = DeEsserMode::Split;
    float freqHz = 6000.0f;
    float q = 2.0f;
    float thresholdDb = -30.0f;
    float rangeDb = 12.0f;
    float attackMs = 0.1f;
    float releaseMs = 50.0f;
    bool operator==(const DeEsserEffect&) const = default;
};

DeEsserEffect makeDefaultDeEsserEffect(std::uint32_t id = 1);
std::vector<DeEsserEffect> defaultDeEsserEffects();
float deEsserStaticGainDb(float levelDb, const DeEsserEffect& effect);
// Sibilance-band level (dBFS peak) for a UI tick: runs the same band-pass
// the audio processor uses on a copy of the tap, measures the band peak,
// and returns its level. The card and editor GR bars were fed broadband
// peak/RMS, so a 440 Hz voice at -6 dB hid a quiet 6 kHz sibilance and the
// bar never moved. Band-pass level is what the processor actually reduces.
float deEsserBandLevelDb(const float* left, const float* right, int frames,
                         const DeEsserEffect& effect, double sampleRateHz, float& stateS1L, float& stateS2L, float& stateS1R, float& stateS2R);

class DeEsserProcessor {
public:
    void prepare(double sampleRate);
    void setEffect(const DeEsserEffect& effect);
    void process(float* left, float* right, int frames) noexcept;
    void reset() noexcept;
    float gainReductionDb() const noexcept { return reductionDb_; }

private:
    double sampleRate_ = 48000.0;
    DeEsserEffect effect_;
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float s1L_ = 0.0f, s2L_ = 0.0f, s1R_ = 0.0f, s2R_ = 0.0f;
    float detB0_ = 1.0f, detB1_ = 0.0f, detB2_ = 0.0f, detA1_ = 0.0f, detA2_ = 0.0f;
    float detS1L_ = 0.0f, detS2L_ = 0.0f, detS1R_ = 0.0f, detS2R_ = 0.0f;
    float envelope_ = 0.0f;
    float gain_ = 1.0f;
    float reductionDb_ = 0.0f;
    void rebuild();
};

} // namespace pluginrack
