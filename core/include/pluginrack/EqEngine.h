#pragma once

#include <vector>

namespace pluginrack {

// Filter types for the fixed six-band channel EQ (RBJ cookbook biquads).
enum class EqType { HighPass, LowShelf, Peak, HighShelf };

struct EqBand {
    EqType type;
    float freqHz;
    float gainDb;
    float q;
    bool enabled;
    bool solo;
    bool operator==(const EqBand&) const = default;
};

// HP 80 Hz, low shelf 200 Hz, peaks 500 Hz / 1 kHz / 3 kHz, high shelf 8 kHz.
// All 0 dB, Q 0.707, all enabled, none solo. (Q is unused for the high-pass.)
std::vector<EqBand> defaultSixBands();

// Fixed six-band stereo parametric EQ, DF-II transposed biquads, one state
// per band per channel. Coefficients are recomputed in prepare()/setBands()
// only (message thread); process() is a multiply-add loop over preallocated
// state and never allocates. Non-finite band parameters bypass their band.
// Solo rule: if any band has solo == true, only soloed and enabled bands run.
class EqEngine {
public:
    void prepare(double sampleRate, int maxBlock);
    void setBands(const std::vector<EqBand>& bands);
    void process(float* L, float* R, int n) noexcept;
    void reset() noexcept;

private:
    static constexpr int kNumBands = 6;

    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float s1L = 0.0f, s2L = 0.0f;
        float s1R = 0.0f, s2R = 0.0f;
    };

    void rebuild();
    static void computeCoeffs(const EqBand& band, double sampleRate, Biquad& out) noexcept;
    static void applyChannel(float* buf, int n, Biquad& f, float& s1, float& s2) noexcept;

    double sampleRate_ = 0.0; // 0.0 until prepared; process() is a no-op before
    int maxBlock_ = 0;
    std::vector<EqBand> bands_;
    Biquad filters_[kNumBands];
    int activeMask_ = 0;
};

} // namespace pluginrack
