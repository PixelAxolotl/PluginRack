#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pluginrack {

// 75% overlap at every size: one full frame of latency, no lookahead.
// 512 samples is the default and reproduces the original block exactly; the
// larger sizes trade latency for frequency resolution.
inline constexpr int kSpectralDenoiserDefaultFftSize = 512;
inline constexpr std::array<int, 4> kSpectralDenoiserFftSizes{512, 1024, 2048, 4096};
inline constexpr int kSpectralDenoiserFftSize = kSpectralDenoiserDefaultFftSize;
inline constexpr int kSpectralDenoiserHopSize = kSpectralDenoiserFftSize / 4;
inline constexpr int kSpectralDenoiserBinCount = kSpectralDenoiserFftSize / 2 + 1;
inline constexpr int kSpectralDenoiserLatencySamples = kSpectralDenoiserFftSize;

inline constexpr int spectralDenoiserHopSize(int fftSize) noexcept { return fftSize / 4; }
inline constexpr int spectralDenoiserBinCount(int fftSize) noexcept { return fftSize / 2 + 1; }

inline constexpr bool isSpectralDenoiserFftSizeSupported(int fftSize) noexcept {
    for (const int supported : kSpectralDenoiserFftSizes) {
        if (fftSize == supported) {
            return true;
        }
    }
    return false;
}

// An unsupported size falls back to the default instead of feeding the FFT a
// non-power-of-two length. Scenes are validated first; this also guards
// effects built by hand.
inline constexpr int spectralDenoiserFftSizeOrDefault(int fftSize) noexcept {
    return isSpectralDenoiserFftSizeSupported(fftSize) ? fftSize
                                                       : kSpectralDenoiserDefaultFftSize;
}

// A learned profile carries one bin per point up to Nyquist, so its length
// names the transform size it was captured with.
inline constexpr bool isSpectralDenoiserProfileSize(std::size_t bins) noexcept {
    for (const int supported : kSpectralDenoiserFftSizes) {
        if (bins == static_cast<std::size_t>(spectralDenoiserBinCount(supported))) {
            return true;
        }
    }
    return false;
}

inline constexpr float kSpectralDenoiserMinReductionDb = 0.0f;
inline constexpr float kSpectralDenoiserMaxReductionDb = 40.0f;
inline constexpr float kSpectralDenoiserDefaultReductionDb = 12.0f;
inline constexpr float kSpectralDenoiserMinSampleRate = 8000.0f;
inline constexpr int kSpectralDenoiserTrimBandCount = 32;
inline constexpr float kSpectralDenoiserMinTrimDb = -18.0f;
inline constexpr float kSpectralDenoiserMaxTrimDb = 18.0f;
inline constexpr float kSpectralDenoiserTrimLowHz = 20.0f;
inline constexpr float kSpectralDenoiserMinThresholdOffsetDb = -18.0f;
inline constexpr float kSpectralDenoiserMaxThresholdOffsetDb = 18.0f;
inline constexpr float kSpectralDenoiserMinMixPercent = 0.0f;
inline constexpr float kSpectralDenoiserMaxMixPercent = 100.0f;
inline constexpr float kSpectralDenoiserMinSpeed = 0.25f;
inline constexpr float kSpectralDenoiserMaxSpeed = 4.0f;
inline constexpr float kSpectralDenoiserMinLearnSeconds = 0.5f;
inline constexpr float kSpectralDenoiserMaxLearnSeconds = 3.0f;
inline constexpr float kSpectralDenoiserMinStereoLinkPercent = 0.0f;
inline constexpr float kSpectralDenoiserMaxStereoLinkPercent = 100.0f;

// Maps a frequency onto one of the log-spaced profile-trim bands. The editor
// and the processor share this so a dragged band covers exactly what is cut.
inline int spectralDenoiserTrimBand(double frequencyHz, double nyquistHz) noexcept {
    if (!(frequencyHz > kSpectralDenoiserTrimLowHz)
        || !(nyquistHz > kSpectralDenoiserTrimLowHz)) {
        return 0;
    }
    const double span = std::log(nyquistHz / kSpectralDenoiserTrimLowHz);
    const double position = std::log(frequencyHz / kSpectralDenoiserTrimLowHz) / span;
    const int band = static_cast<int>(position * kSpectralDenoiserTrimBandCount);
    if (band < 0) {
        return 0;
    }
    return band >= kSpectralDenoiserTrimBandCount ? kSpectralDenoiserTrimBandCount - 1 : band;
}

inline float spectralDenoiserTrimDb(const std::vector<float>& trims, int band) noexcept {
    if (band < 0 || band >= kSpectralDenoiserTrimBandCount
        || trims.size() != static_cast<std::size_t>(kSpectralDenoiserTrimBandCount)) {
        return 0.0f;
    }
    return trims[static_cast<std::size_t>(band)];
}

// Interpolates the trim curve at an arbitrary frequency. Bands narrower than
// the FFT resolution have no bin centre of their own, so their trims still
// reach the surrounding bins through this curve.
inline float spectralDenoiserTrimDbAt(const std::vector<float>& trims,
                                      double frequencyHz,
                                      double nyquistHz) noexcept {
    if (trims.size() != static_cast<std::size_t>(kSpectralDenoiserTrimBandCount)
        || !(nyquistHz > kSpectralDenoiserTrimLowHz)) {
        return 0.0f;
    }
    const double span = std::log(nyquistHz / kSpectralDenoiserTrimLowHz);
    double position = 0.0;
    if (frequencyHz > kSpectralDenoiserTrimLowHz) {
        position = std::log(frequencyHz / kSpectralDenoiserTrimLowHz) / span
            * kSpectralDenoiserTrimBandCount;
    }
    // Band b owns [b, b+1), so its value lives at the band centre (b + 0.5).
    // Interpolating through those points keeps each band's trim intact at its
    // centre while blending across the edges.
    double curvePosition = position - 0.5;
    curvePosition = std::clamp(curvePosition, 0.0,
                               static_cast<double>(kSpectralDenoiserTrimBandCount - 1));
    const auto index = static_cast<std::size_t>(curvePosition);
    const auto next = std::min(index + 1,
                               static_cast<std::size_t>(kSpectralDenoiserTrimBandCount - 1));
    const auto fraction = static_cast<float>(curvePosition - static_cast<double>(index));
    return trims[index] + (trims[next] - trims[index]) * fraction;
}

enum class SpectralDenoiserMode {
    Learn = 0,   // use the captured noise profile without tracking
    Adaptive = 1 // track the noise floor while reducing
};

struct SpectralDenoiserEffect {
    std::uint32_t id = 0;
    bool bypassed = false;
    SpectralDenoiserMode mode = SpectralDenoiserMode::Adaptive;
    float reductionDb = kSpectralDenoiserDefaultReductionDb;
    // Learned noise power per FFT bin (0..Nyquist). Empty when never learned.
    std::vector<float> noiseProfile;
    double profileSampleRate = 0.0;
    // Optional per-band offset applied to the noise estimate. Empty means
    // untouched; otherwise exactly kSpectralDenoiserTrimBandCount entries.
    std::vector<float> profileTrimDb;
    // Shifts the whole threshold up or down without changing what is learned.
    // Positive values assume more noise and cut harder.
    float thresholdOffsetDb = 0.0f;
    // Wet/dry balance. The dry side is the delayed input, so latency is fixed.
    float mixPercent = kSpectralDenoiserMaxMixPercent;
    // Monitor-only: plays what the denoiser removes. Never saved with a scene.
    bool listen = false;
    // Multiplies both the attack and release rates; higher reacts faster.
    float speed = 1.0f;
    // How much noise the Learn pass averages, in seconds.
    float learnSeconds = 1.0f;
    // 100 keeps one mask for both sides; lower blends toward per-side gains.
    float stereoLinkPercent = kSpectralDenoiserMaxStereoLinkPercent;
    // Transform size in samples: 512, 1024, 2048 or 4096. The hop follows at
    // 75% overlap, so latency equals this size.
    int fftSize = kSpectralDenoiserDefaultFftSize;

    bool hasProfile() const noexcept {
        return profileSampleRate > 0.0
            && isSpectralDenoiserProfileSize(noiseProfile.size());
    }

    bool operator==(const SpectralDenoiserEffect&) const = default;
};

SpectralDenoiserEffect makeDefaultSpectralDenoiserEffect(std::uint32_t id = 1);

// One full frame of delay, so the reported latency follows the size.
inline int spectralDenoiserLatencySamples(const SpectralDenoiserEffect& effect) noexcept {
    return spectralDenoiserFftSizeOrDefault(effect.fftSize);
}

// Streaming STFT denoiser. All buffers are allocated in prepare(); process()
// never allocates. A change of FFT size in setEffect() reallocates and resets
// the same way, which is safe because setEffect() runs on the message thread
// under the engine's state lock. The two channels share one complex FFT per
// hop (left in the real part, right in the imaginary part) and are split back
// before the masks.
// One gain mask is shared by default; the stereo link control blends toward
// per-side masks that use the same noise estimate.
class SpectralDenoiserProcessor {
public:
    SpectralDenoiserProcessor() = default;
    SpectralDenoiserProcessor(const SpectralDenoiserProcessor& other);
    SpectralDenoiserProcessor& operator=(const SpectralDenoiserProcessor& other);
    ~SpectralDenoiserProcessor() = default;

    void prepare(double sampleRate);
    void setEffect(const SpectralDenoiserEffect& effect);
    void process(float* left, float* right, int frames) noexcept;
    void reset() noexcept;

    // Captures the noise floor for one second of processed audio. The profile
    // is collected on the audio thread and handed over atomically.
    void startLearning() noexcept;
    bool isLearning() const noexcept {
        return learning_.load(std::memory_order_acquire)
            || learnRequest_.load(std::memory_order_acquire);
    }
    float learnProgress() const noexcept;
    bool takeLearnedProfile(std::vector<float>& profile, double& sampleRate) noexcept;

    // Allocated transform size; may differ from effect_.fftSize only until
    // the next setEffect/prepare.
    int latencySamples() const noexcept { return fftSize_; }
    float currentReductionDb() const noexcept {
        return currentReductionDb_.load(std::memory_order_relaxed);
    }

    // Live reduction per profile-trim band, matching what the editor draws.
    // Bands run low to high frequency; values are 0..40 dB.
    float bandReductionDb(int band) const noexcept {
        if (band < 0 || band >= kSpectralDenoiserTrimBandCount) {
            return 0.0f;
        }
        return bandReductionDb_[static_cast<std::size_t>(band)].load(std::memory_order_relaxed);
    }

private:
    void rebuildNoiseFromProfile();
    void rebuildTrimGains();
    void updateSmoothing();
    void processFrame() noexcept;
    void fft(std::vector<float>& re, std::vector<float>& im, bool inverse) const noexcept;

    double sampleRate_ = 48000.0;
    SpectralDenoiserEffect effect_{};
    bool prepared_ = false;

    // Buffer geometry for the size the vectors below were allocated at.
    int fftSize_ = kSpectralDenoiserDefaultFftSize;
    int binCount_ = kSpectralDenoiserBinCount;
    int hopSize_ = kSpectralDenoiserHopSize;
    int overlapSize_ = kSpectralDenoiserDefaultFftSize * 4;
    std::size_t overlapMask_ = static_cast<std::size_t>(kSpectralDenoiserDefaultFftSize) * 4 - 1;

    std::vector<float> window_;
    std::vector<float> inputLeft_;
    std::vector<float> inputRight_;
    std::vector<float> overlapLeft_;
    std::vector<float> overlapRight_;
    std::vector<float> reLeft_;
    std::vector<float> imLeft_;
    std::vector<float> reRight_;
    std::vector<float> imRight_;
    std::vector<float> noise_;
    std::vector<float> trimGain_;
    std::vector<float> rawGain_;
    std::vector<float> frequencyGain_;
    std::vector<float> smoothGain_;
    std::vector<float> rawGainLeft_;
    std::vector<float> rawGainRight_;
    std::vector<float> frequencyGainLeft_;
    std::vector<float> frequencyGainRight_;
    std::vector<float> smoothGainLeft_;
    std::vector<float> smoothGainRight_;
    std::vector<double> learnSum_;
    std::vector<float> pendingProfile_;

    std::size_t inputWrite_ = 0;
    std::uint64_t inputSamples_ = 0;
    int hopCounter_ = 0;
    bool noiseSeeded_ = false;
    float overlapNorm_ = 1.0f;
    float attackCoeff_ = 0.2f;
    float releaseCoeff_ = 0.05f;
    int learnTargetFrames_ = 375;
    std::atomic<int> learnFrames_{0};

    std::atomic<bool> learnRequest_{false};
    std::atomic<bool> learning_{false};
    std::atomic<bool> profileReady_{false};
    std::atomic<float> currentReductionDb_{0.0f};
    std::array<std::atomic<float>, kSpectralDenoiserTrimBandCount> bandReductionDb_{};
};

} // namespace pluginrack
