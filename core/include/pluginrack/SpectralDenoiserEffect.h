#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace pluginrack {

// 512-point frame with 75% overlap: one full frame of latency, no lookahead.
inline constexpr int kSpectralDenoiserFftSize = 512;
inline constexpr int kSpectralDenoiserHopSize = 128;
inline constexpr int kSpectralDenoiserBinCount = kSpectralDenoiserFftSize / 2 + 1;
inline constexpr int kSpectralDenoiserLatencySamples = kSpectralDenoiserFftSize;
inline constexpr float kSpectralDenoiserMinReductionDb = 0.0f;
inline constexpr float kSpectralDenoiserMaxReductionDb = 40.0f;
inline constexpr float kSpectralDenoiserDefaultReductionDb = 12.0f;
inline constexpr float kSpectralDenoiserMinSampleRate = 8000.0f;

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

    bool hasProfile() const noexcept {
        return profileSampleRate > 0.0
            && noiseProfile.size() == static_cast<std::size_t>(kSpectralDenoiserBinCount);
    }

    bool operator==(const SpectralDenoiserEffect&) const = default;
};

SpectralDenoiserEffect makeDefaultSpectralDenoiserEffect(std::uint32_t id = 1);

// Streaming STFT denoiser. All buffers are allocated in prepare(); process()
// never allocates. Stereo is linked: one gain mask is applied to both sides.
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

    static constexpr int latencySamples() noexcept { return kSpectralDenoiserLatencySamples; }
    float currentReductionDb() const noexcept {
        return currentReductionDb_.load(std::memory_order_relaxed);
    }

private:
    void rebuildNoiseFromProfile();
    void processFrame() noexcept;
    void fft(std::vector<float>& re, std::vector<float>& im, bool inverse) const noexcept;

    double sampleRate_ = 48000.0;
    SpectralDenoiserEffect effect_{};
    bool prepared_ = false;

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
    std::vector<float> rawGain_;
    std::vector<float> frequencyGain_;
    std::vector<float> smoothGain_;
    std::vector<double> learnSum_;
    std::vector<float> pendingProfile_;

    std::size_t inputWrite_ = 0;
    std::uint64_t inputSamples_ = 0;
    int hopCounter_ = 0;
    bool noiseSeeded_ = false;
    float overlapNorm_ = 1.0f;
    float attackCoeff_ = 0.2f;
    float releaseCoeff_ = 0.05f;
    std::atomic<int> learnFrames_{0};

    std::atomic<bool> learnRequest_{false};
    std::atomic<bool> learning_{false};
    std::atomic<bool> profileReady_{false};
    std::atomic<float> currentReductionDb_{0.0f};
};

} // namespace pluginrack
