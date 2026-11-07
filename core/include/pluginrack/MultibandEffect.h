#pragma once

#include <cstdint>
#include <vector>

namespace pluginrack {

enum class MultibandMode { Compress, Expand };

struct MultibandBand {
    bool bypassed = false;
    bool soloed = false;
    bool enabled = true;
    MultibandMode mode = MultibandMode::Compress;
    float thresholdDb = -18.0f;
    float ratio = 4.0f;
    float attackMs = 10.0f;
    float releaseMs = 100.0f;
    float makeupDb = 0.0f;
    float inputGainDb = 0.0f;
    float kneeDb = 0.0f;
    bool operator==(const MultibandBand&) const = default;
};

struct MultibandEffect {
    std::uint32_t id = 0;
    bool bypassed = false;
    bool autoMakeup = false;
    std::vector<MultibandBand> bands;
    std::vector<float> crossovers;
    bool operator==(const MultibandEffect&) const = default;
};

MultibandEffect makeDefaultMultibandEffect(std::uint32_t id = 1);
std::vector<MultibandEffect> defaultMultibandEffects();
float multibandStaticGainDb(float levelDb, const MultibandBand& band);

class MultibandProcessor {
public:
    void prepare(double sampleRate);
    void setEffect(const MultibandEffect& effect);
    void process(float* left, float* right, int frames) noexcept;
    void reset() noexcept;
    const std::vector<float>& gainReductionDb() const noexcept { return reductions_; }
    float autoMakeupDb() const noexcept { return autoMakeupDb_; }

private:
    struct BiquadState { float s1L = 0.0f, s2L = 0.0f, s1R = 0.0f, s2R = 0.0f; };
    struct Envelope { float env = 0.0f; float gain = 1.0f; };
    double sampleRate_ = 48000.0;
    MultibandEffect effect_;
    std::vector<BiquadState> lpStates_;
    std::vector<BiquadState> hpStates_;
    std::vector<Envelope> envelopes_;
    std::vector<float> reductions_;
    std::vector<float> bufL_;
    std::vector<float> bufR_;
    std::vector<float> outL_;
    std::vector<float> outR_;
    std::vector<float> lpL_;
    std::vector<float> lpR_;
    std::vector<float> hpL_;
    std::vector<float> hpR_;
    float autoMakeupDb_ = 0.0f;
    int activeSolo_ = -1;
    bool prepared_ = false;
    void rebuild();
    void ensureScratch(int frames);
};

} // namespace pluginrack
