#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace pluginrack {

inline constexpr float kFeedbackSuppressorMinSensitivityDb = -80.0f;
inline constexpr float kFeedbackSuppressorMaxSensitivityDb = 0.0f;
inline constexpr float kFeedbackSuppressorDefaultSensitivityDb = -40.0f;
inline constexpr float kFeedbackSuppressorMinDetectorHz = 20.0f;
inline constexpr float kFeedbackSuppressorMaxDetectorHz = 20000.0f;
inline constexpr float kFeedbackSuppressorMaxHighPassHz = 200.0f;
inline constexpr float kFeedbackSuppressorMinLowPassHz = 4000.0f;
// The knob's minimum position means "no cutoff": the detector's 48 dB/oct
// high-pass is replaced by a single 5 Hz DC-blocking section, so nothing in
// the audible bass range is filtered away. Anything above the minimum filters
// normally.
inline constexpr float kFeedbackSuppressorDefaultHighPassHz = kFeedbackSuppressorMinDetectorHz;
inline constexpr float kFeedbackSuppressorDcBlockerHz = 5.0f;
// At or below this the detector stops high-passing the analysis signal.
inline constexpr float kFeedbackSuppressorHighPassBypassHz = 20.0f;
inline constexpr float kFeedbackSuppressorDefaultLowPassHz = 14000.0f;
inline constexpr float kFeedbackSuppressorDetectorGapHz = 20.0f;
inline constexpr float kFeedbackSuppressorMinAttenuationDb = 0.0f;
inline constexpr float kFeedbackSuppressorMaxAttenuationDb = 24.0f;
inline constexpr float kFeedbackSuppressorDefaultAttenuationDb = 12.0f;
inline constexpr float kFeedbackSuppressorMinGainReductionMultiplier = 0.25f;
inline constexpr float kFeedbackSuppressorMaxGainReductionMultiplier = 4.0f;
inline constexpr float kFeedbackSuppressorDefaultGainReductionMultiplier = 1.0f;
inline constexpr float kFeedbackSuppressorMinAttackMs = 0.0f;
inline constexpr float kFeedbackSuppressorMaxAttackMs = 100.0f;
inline constexpr float kFeedbackSuppressorDefaultAttackMs = 20.0f;
inline constexpr float kFeedbackSuppressorMinHoldMs = 0.0f;
inline constexpr float kFeedbackSuppressorMaxHoldMs = 2000.0f;
inline constexpr float kFeedbackSuppressorDefaultHoldMs = 300.0f;
inline constexpr float kFeedbackSuppressorMinReleaseMs = 10.0f;
inline constexpr float kFeedbackSuppressorMaxReleaseMs = 2000.0f;
inline constexpr float kFeedbackSuppressorDefaultReleaseMs = 300.0f;
inline constexpr float kFeedbackSuppressorMinDetectionDelayMs = 0.0f;
inline constexpr float kFeedbackSuppressorMaxDetectionDelayMs = 1000.0f;
inline constexpr float kFeedbackSuppressorDefaultDetectionDelayMs = 300.0f;
inline constexpr int kFeedbackSuppressorMinCuts = 1;
inline constexpr int kFeedbackSuppressorMaxCuts = 8;
inline constexpr int kFeedbackSuppressorDefaultCuts = 4;
inline constexpr int kFeedbackSuppressorAnalysisSize = 2048;

// The detector gate is clocked by samples, not by host callbacks. Judging a
// whole callback at once lets one quiet 64-frame block near a bass tone's zero
// crossing look like silence and restart the 8192-sample analysis window.
// Energy is averaged over a 50 ms rolling window, the gate closes only after a
// 120 ms dwell 3 dB below the open threshold, and only a two-second silence
// discards the analysis history.
inline constexpr float kFeedbackSuppressorGateWindowSeconds = 0.05f;
inline constexpr float kFeedbackSuppressorGateCloseRatio = 0.70794578f;  // -3 dB
inline constexpr float kFeedbackSuppressorGateCloseSeconds = 0.12f;
inline constexpr float kFeedbackSuppressorGateResetSeconds = 2.0f;
inline constexpr int kFeedbackSuppressorMaxGateWindowSamples = 16384;

// The Strength knob is a cut count, but scenes store it as the original 0..1
// float so sessions saved before the knob changed still load correctly.
inline int feedbackSuppressorCutCount(float strength) noexcept {
    if (!(strength >= 0.05f)) return 0;
    return std::clamp(static_cast<int>(std::lround(strength * 8.0f)),
                      kFeedbackSuppressorMinCuts, kFeedbackSuppressorMaxCuts);
}

inline float feedbackSuppressorStrengthForCuts(int cuts) noexcept {
    return static_cast<float>(std::clamp(cuts, kFeedbackSuppressorMinCuts,
                                         kFeedbackSuppressorMaxCuts))
         / static_cast<float>(kFeedbackSuppressorMaxCuts);
}

// Wider cuts as the requested depth increases: a 12 dB cut keeps the
// previous 32-ish Q at 1 kHz, shallow cuts stay surgical, and deep cuts widen
// out. Q also rises with frequency (square-root of f/1 kHz, capped), so a
// 5 kHz cut covers fewer octaves than a 1 kHz cut instead of every cut looking
// equally wide on the log RTA. At low frequencies the analyzer's bins are
// wider than a high-Q notch, so cap Q there as well: otherwise a peak can sit
// between two bins and the cut lands beside it instead of on it.
inline float feedbackSuppressorNotchQ(float attenuationDb,
                                      float frequencyHz = 0.0f,
                                      float analysisBinWidthHz = 0.0f) noexcept {
    const float depth = std::max(3.0f, attenuationDb);
    const float frequencyScale = frequencyHz > 0.0f
        ? std::sqrt(std::max(frequencyHz, 20.0f) / 1000.0f)
        : 1.0f;
    float q = std::clamp(32.0f * (12.0f / depth), 6.0f, 48.0f) * frequencyScale;
    if (frequencyHz > 0.0f && analysisBinWidthHz > 0.0f) {
        const float minBandwidth = std::max(1.0f, analysisBinWidthHz * 0.5f);
        q = std::min(q, frequencyHz / minBandwidth);
    }
    return std::clamp(q, 3.0f, 48.0f);
}

struct FeedbackSuppressorEffect {
    std::uint32_t id = 0;
    bool bypassed = false;
    // 0..1 storage for the number of cuts (1..8). The editor shows the cut
    // count directly; keeping this normalized preserves older scenes.
    float strength = feedbackSuppressorStrengthForCuts(kFeedbackSuppressorDefaultCuts);
    // Depth of each cut in dB. Light cuts stay narrow; deeper cuts widen
    // automatically via feedbackSuppressorNotchQ().
    float attenuationDb = kFeedbackSuppressorDefaultAttenuationDb;
    // Multiplies the computed gain reduction before it is capped by
    // Attenuation. 1.0 follows the detector normally; lower is gentler and
    // higher reaches the maximum cut sooner.
    float gainReductionMultiplier = kFeedbackSuppressorDefaultGainReductionMultiplier;
    // Envelope for engaging a new notch and for keeping it after the ringing
    // stops. Release is the fade back to dry after Hold expires.
    float attackMs = kFeedbackSuppressorDefaultAttackMs;
    float holdMs = kFeedbackSuppressorDefaultHoldMs;
    float releaseMs = kFeedbackSuppressorDefaultReleaseMs;
    // A new frequency must persist this long before it becomes a cut. This
    // keeps short voice peaks from being mistaken for feedback.
    float detectionDelayMs = kFeedbackSuppressorDefaultDetectionDelayMs;
    // Below this input level (dBFS RMS) the detector and filters are fully
    // bypassed, so quiet room noise can never grow a notch.
    float inputSensitivityDb = kFeedbackSuppressorDefaultSensitivityDb;
    // The detector only looks for feedback between these two frequencies.
    // Cutting outside the voice band mostly removes musical content, so the
    // defaults cover speech/singing rather than the full audio range.
    float detectorHighPassHz = kFeedbackSuppressorDefaultHighPassHz;
    float detectorLowPassHz = kFeedbackSuppressorDefaultLowPassHz;
    bool operator==(const FeedbackSuppressorEffect&) const = default;
};

FeedbackSuppressorEffect makeDefaultFeedbackSuppressorEffect(std::uint32_t id = 1);
std::vector<FeedbackSuppressorEffect> defaultFeedbackSuppressorEffects();
float feedbackSuppressorStaticGainDb(float levelDb, const FeedbackSuppressorEffect& effect);

class FeedbackSuppressorProcessor {
public:
    FeedbackSuppressorProcessor() = default;
    // The published-notch atomics make the implicit copy/move operations
    // deleted, but MixerEngine stores these in a growing std::vector. Copy the
    // audio state explicitly instead; this only happens when the engine
    // rebuilds a strip, never on the render thread.
    FeedbackSuppressorProcessor(const FeedbackSuppressorProcessor& other);
    FeedbackSuppressorProcessor& operator=(const FeedbackSuppressorProcessor& other);
    void prepare(double sampleRate);
    void setEffect(const FeedbackSuppressorEffect& effect);
    void process(float* left, float* right, int frames) noexcept;
    void reset() noexcept;
    float gainReductionDb() const noexcept { return reductionDb_; }
    // Lock-free snapshot for the editor's response graph. Writes happen on the
    // audio thread; this only reads relaxed atomics, so a UI paint can never
    // block or tear the processor's notch state.
    int copyActiveNotches(float* dest, int max) const noexcept;
    int copyActiveNotches(float* freqs, float* depthsDb, int max) const noexcept;

    // Detector bookkeeping for tests and tuning. Written on the audio thread,
    // read by the same thread (or a test) after rendering; no tearing concern
    // in practice because a snapshot is only informational.
    struct Stats {
        std::uint32_t gateCloses = 0;
        std::uint32_t gateResets = 0;
        std::uint32_t promotions = 0;
        std::uint32_t preemptions = 0;
        std::uint32_t candidateRejections = 0;
    };
    const Stats& stats() const noexcept { return stats_; }

private:
    static constexpr int kMaxNotches = 8;
    static constexpr int kHighAnalysisSize = kFeedbackSuppressorAnalysisSize;
    static constexpr int kLowAnalysisSize = 8192;
    static constexpr float kAnalysisSplitHz = 500.0f;
    // The high-band FFT window is 2048 samples, but it can slide every 512
    // samples (about 10.7 ms at 48 kHz). The low-band FFT uses 8192 samples
    // (~5.9 Hz bins) for usable bass resolution while the high band keeps its
    // fast 2048-sample response.
    static constexpr int kAnalysisInterval = 512;

    struct Notch {
        bool active = false;
        float freq = 1000.0f;
        float depthDb = 0.0f;
        float targetDepthDb = 0.0f;
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float z1L = 0.0f, z2L = 0.0f;
        float z1R = 0.0f, z2R = 0.0f;
        float envelope = 0.0f;
        float target = 0.0f;
        int holdSamples = 0;
        float binWidthHz = 23.4f;
        float levelDb = -120.0f;
    };

    struct Detection {
        float freq = 1000.0f;
        float depthDb = 0.0f;
        float tonalityDb = 0.0f;
        float levelDb = -120.0f;
        float leftBin = 0.0f;
        float rightBin = 0.0f;
        float binWidthHz = 0.0f;
    };

    struct Peak {
        float magnitude = 0.0f;
        float bin = 0.0f;
        float leftBin = 0.0f;
        float rightBin = 0.0f;
        float tonalityDb = 0.0f;
        float levelDb = -120.0f;
    };

    struct Candidate {
        float freq = 0.0f;
        float depthDb = 0.0f;
        float tonalityDb = 0.0f;
        float firstLevelDb = -120.0f;
        float lastLevelDb = -120.0f;
        int hits = 0;
        int misses = 0;
        int lastSeenPass = -1;
        float binWidthHz = 0.0f;
    };

    double sampleRate_ = 48000.0;
    FeedbackSuppressorEffect effect_{};
    std::array<Notch, kMaxNotches> notches_{};
    std::array<Candidate, kMaxNotches> candidates_{};
    float attackStep_ = 1.0f;
    float releaseStep_ = 1.0f;
    int holdSamples_ = 0;
    std::array<std::atomic<float>, kMaxNotches> publishedFreqs_{};
    std::array<std::atomic<float>, kMaxNotches> publishedDepths_{};
    std::atomic<int> publishedCount_{0};
    // One ring per input channel: the low band compares per-channel power so
    // anti-phase feedback cannot cancel in a mono sum.
    std::array<std::vector<float>, 2> analysisBuffer_;
    int analysisWrite_ = 0;
    int analysisCount_ = 0;
    int framesSinceAnalysis_ = 0;
    std::vector<float> fftReal_;
    std::vector<float> fftImag_;
    std::vector<float> mags_;
    std::vector<float> fftLowReal_;
    std::vector<float> fftLowImag_;
    std::vector<float> magsLow_;
    std::vector<Peak> peaks_;
    std::vector<Detection> detections_;
    int analysisPass_ = 0;
    float reductionDb_ = 0.0f;
    float inputRmsAccum_ = 0.0f;
    float outputRmsAccum_ = 0.0f;
    int rmsCount_ = 0;
    float sensitivityLinear_ = 0.0031622777f;
    bool gateOpen_ = false;
    // Sample-clocked gate: a short ring of per-sample energy plus a running
    // sum, so the decision is independent of the host's block size.
    std::vector<float> gateEnergy_;
    int gateWrite_ = 0;
    int gateWindow_ = 1;
    double gateEnergySum_ = 0.0;
    double gateOpenEnergy_ = 0.0;
    double gateCloseEnergy_ = 0.0;
    int gateCloseDwell_ = 0;
    int gateResetDwell_ = 0;
    int gateClosedSamples_ = 0;
    // Samples accumulated in the rolling energy window. Opening uses the
    // mean over what has been collected so far so a fresh detector still
    // engages on the first loud block; closing always needs the full window.
    int gateFilled_ = 0;
    Stats stats_{};
    // Four cascaded second-order sections give a 48 dB/oct detector filter.
    // The Q values below form an 8th-order Butterworth response so the band
    // edge is steep without rippling into the pass band.
    std::array<float, 4> hpB0_{1.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> hpB1_{};
    std::array<float, 4> hpB2_{};
    std::array<float, 4> hpA1_{};
    std::array<float, 4> hpA2_{};
    std::array<std::array<float, 4>, 2> hpZ1_{};
    std::array<std::array<float, 4>, 2> hpZ2_{};
    // Mirror the HPF on the detector's upper edge. Filtering only the FFT
    // search bins still lets a loud out-of-band tone leak into in-band bins
    // through the analysis window and look like a valid resonance.
    std::array<float, 4> lpB0_{1.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> lpB1_{};
    std::array<float, 4> lpB2_{};
    std::array<float, 4> lpA1_{};
    std::array<float, 4> lpA2_{};
    std::array<std::array<float, 4>, 2> lpZ1_{};
    std::array<std::array<float, 4>, 2> lpZ2_{};

    void analyze();
    void analyzeSpectrum(int fftSize, const std::vector<float>& mags,
                         float bandLowHz, float bandHighHz, int budget);
    void updateNotches(const std::vector<Detection>& detections,
                       bool ageCandidates = true);
    void updateNotchCoefficients(Notch& notch) const noexcept;
    void updateAnalysisHighPass() noexcept;
    void resetAnalysisHighPass() noexcept;
    void updateAnalysisLowPass() noexcept;
    void resetAnalysisLowPass() noexcept;
    void refreshEnvelopeSteps() noexcept;
    void refreshGateThresholds() noexcept;
    void resetGate() noexcept;
    void clearAnalysisHistory() noexcept;
    void clearCandidates() noexcept;
    void closeGate() noexcept;
};

} // namespace pluginrack