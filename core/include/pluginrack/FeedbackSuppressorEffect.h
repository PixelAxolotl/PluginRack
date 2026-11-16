#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace pluginrack {

struct FeedbackSuppressorEffect {
    std::uint32_t id = 0;
    bool bypassed = false;
    // 0..1 overall amount. Drives how many narrow notches are allowed and how
    // aggressively they cut. A single knob, matching the DeFeedback.ai UX.
    float strength = 0.5f;
    bool operator==(const FeedbackSuppressorEffect&) const = default;
};

FeedbackSuppressorEffect makeDefaultFeedbackSuppressorEffect(std::uint32_t id = 1);
std::vector<FeedbackSuppressorEffect> defaultFeedbackSuppressorEffects();
float feedbackSuppressorStaticGainDb(float levelDb, const FeedbackSuppressorEffect& effect);

class FeedbackSuppressorProcessor {
public:
    void prepare(double sampleRate);
    void setEffect(const FeedbackSuppressorEffect& effect);
    void process(float* left, float* right, int frames) noexcept;
    void reset() noexcept;
    float gainReductionDb() const noexcept { return reductionDb_; }

private:
    static constexpr int kMaxNotches = 8;
    static constexpr int kAnalysisSize = 2048;
    static constexpr int kAnalysisInterval = 1024;

    struct Notch {
        bool active = false;
        float freq = 1000.0f;
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float z1L = 0.0f, z2L = 0.0f;
        float z1R = 0.0f, z2R = 0.0f;
    };

    double sampleRate_ = 48000.0;
    FeedbackSuppressorEffect effect_{};
    std::array<Notch, kMaxNotches> notches_{};
    std::vector<float> analysisBuffer_;
    int analysisWrite_ = 0;
    int analysisCount_ = 0;
    int framesSinceAnalysis_ = 0;
    std::vector<float> fftReal_;
    std::vector<float> fftImag_;
    float reductionDb_ = 0.0f;
    float inputRmsAccum_ = 0.0f;
    float outputRmsAccum_ = 0.0f;
    int rmsCount_ = 0;

    void analyze();
    void updateNotches(const std::vector<float>& peakFreqs);
};

} // namespace pluginrack