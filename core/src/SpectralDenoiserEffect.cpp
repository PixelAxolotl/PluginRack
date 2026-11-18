#include "pluginrack/SpectralDenoiserEffect.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "pluginrack/DspMath.h"

namespace pluginrack {
namespace {

constexpr std::size_t kOverlapSize = static_cast<std::size_t>(kSpectralDenoiserFftSize) * 4;
constexpr std::size_t kOverlapMask = kOverlapSize - 1;
constexpr float kSilencePower = 1.0e-12f;

float clampReduction(float db) noexcept {
    if (!std::isfinite(db)) {
        return kSpectralDenoiserDefaultReductionDb;
    }
    return std::clamp(db, kSpectralDenoiserMinReductionDb, kSpectralDenoiserMaxReductionDb);
}

int learnFrameTarget(double sampleRate) noexcept {
    const auto frames = static_cast<int>(std::lround(sampleRate / kSpectralDenoiserHopSize));
    return std::max(frames, 1);
}

} // namespace

SpectralDenoiserEffect makeDefaultSpectralDenoiserEffect(std::uint32_t id) {
    SpectralDenoiserEffect effect;
    effect.id = id;
    return effect;
}

SpectralDenoiserProcessor::SpectralDenoiserProcessor(const SpectralDenoiserProcessor& other) {
    *this = other;
}

SpectralDenoiserProcessor& SpectralDenoiserProcessor::operator=(const SpectralDenoiserProcessor& other) {
    if (this == &other) {
        return *this;
    }
    sampleRate_ = other.sampleRate_;
    effect_ = other.effect_;
    prepared_ = other.prepared_;
    window_ = other.window_;
    inputLeft_ = other.inputLeft_;
    inputRight_ = other.inputRight_;
    overlapLeft_ = other.overlapLeft_;
    overlapRight_ = other.overlapRight_;
    reLeft_ = other.reLeft_;
    imLeft_ = other.imLeft_;
    reRight_ = other.reRight_;
    imRight_ = other.imRight_;
    noise_ = other.noise_;
    rawGain_ = other.rawGain_;
    frequencyGain_ = other.frequencyGain_;
    smoothGain_ = other.smoothGain_;
    learnSum_ = other.learnSum_;
    pendingProfile_ = other.pendingProfile_;
    inputWrite_ = other.inputWrite_;
    inputSamples_ = other.inputSamples_;
    hopCounter_ = other.hopCounter_;
    noiseSeeded_ = other.noiseSeeded_;
    learnFrames_.store(other.learnFrames_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    overlapNorm_ = other.overlapNorm_;
    attackCoeff_ = other.attackCoeff_;
    releaseCoeff_ = other.releaseCoeff_;
    learnRequest_.store(other.learnRequest_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    learning_.store(other.learning_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    profileReady_.store(other.profileReady_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    currentReductionDb_.store(other.currentReductionDb_.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
    return *this;
}

void SpectralDenoiserProcessor::prepare(double sampleRate) {
    sampleRate_ = std::max(sampleRate, static_cast<double>(kSpectralDenoiserMinSampleRate));
    const auto fftSize = static_cast<std::size_t>(kSpectralDenoiserFftSize);
    const auto bins = static_cast<std::size_t>(kSpectralDenoiserBinCount);

    window_.assign(fftSize, 0.0f);
    double windowSumSquares = 0.0;
    for (std::size_t n = 0; n < fftSize; ++n) {
        const auto phase = 2.0 * dsp::kPi * static_cast<double>(n) / static_cast<double>(fftSize);
        const auto value = 0.5 - 0.5 * std::cos(phase);
        window_[n] = static_cast<float>(value);
        windowSumSquares += value * value;
    }
    overlapNorm_ = windowSumSquares > 0.0
        ? static_cast<float>(static_cast<double>(kSpectralDenoiserHopSize) / windowSumSquares)
        : 1.0f;

    inputLeft_.assign(fftSize, 0.0f);
    inputRight_.assign(fftSize, 0.0f);
    overlapLeft_.assign(kOverlapSize, 0.0f);
    overlapRight_.assign(kOverlapSize, 0.0f);
    reLeft_.assign(fftSize, 0.0f);
    imLeft_.assign(fftSize, 0.0f);
    reRight_.assign(fftSize, 0.0f);
    imRight_.assign(fftSize, 0.0f);
    noise_.assign(bins, 0.0f);
    rawGain_.assign(bins, 1.0f);
    frequencyGain_.assign(bins, 1.0f);
    smoothGain_.assign(bins, 1.0f);
    learnSum_.assign(bins, 0.0);
    pendingProfile_.assign(bins, 0.0f);

    // Frames arrive every hop; express the time constants in frames.
    const auto frameRate = static_cast<double>(sampleRate_) / kSpectralDenoiserHopSize;
    attackCoeff_ = dsp::smoothingCoeff(frameRate, 10.0f);
    releaseCoeff_ = dsp::smoothingCoeff(frameRate, 60.0f);

    prepared_ = true;
    rebuildNoiseFromProfile();
    reset();
}

void SpectralDenoiserProcessor::setEffect(const SpectralDenoiserEffect& effect) {
    effect_ = effect;
    if (prepared_) {
        rebuildNoiseFromProfile();
    }
}

void SpectralDenoiserProcessor::rebuildNoiseFromProfile() {
    std::fill(noise_.begin(), noise_.end(), 0.0f);
    noiseSeeded_ = false;
    if (!effect_.hasProfile()) {
        return;
    }

    const double sourceRate = effect_.profileSampleRate;
    const auto bins = static_cast<std::size_t>(kSpectralDenoiserBinCount);
    for (std::size_t k = 0; k < bins; ++k) {
        // Map this bin's frequency back to the bins captured at the profile rate.
        const double position = static_cast<double>(k) * sampleRate_ / sourceRate;
        if (position >= static_cast<double>(bins - 1)) {
            noise_[k] = std::max(effect_.noiseProfile[bins - 1], 0.0f);
            continue;
        }
        const auto index = static_cast<std::size_t>(position);
        const auto fraction = static_cast<float>(position - static_cast<double>(index));
        const float a = std::max(effect_.noiseProfile[index], 0.0f);
        const float b = std::max(effect_.noiseProfile[index + 1], 0.0f);
        noise_[k] = a + (b - a) * fraction;
    }
    noiseSeeded_ = true;
}

void SpectralDenoiserProcessor::reset() noexcept {
    std::fill(inputLeft_.begin(), inputLeft_.end(), 0.0f);
    std::fill(inputRight_.begin(), inputRight_.end(), 0.0f);
    std::fill(overlapLeft_.begin(), overlapLeft_.end(), 0.0f);
    std::fill(overlapRight_.begin(), overlapRight_.end(), 0.0f);
    std::fill(smoothGain_.begin(), smoothGain_.end(), 1.0f);
    std::fill(rawGain_.begin(), rawGain_.end(), 1.0f);
    std::fill(frequencyGain_.begin(), frequencyGain_.end(), 1.0f);
    std::fill(learnSum_.begin(), learnSum_.end(), 0.0);
    inputWrite_ = 0;
    inputSamples_ = 0;
    hopCounter_ = 0;
    learnFrames_ = 0;
    learnRequest_.store(false, std::memory_order_relaxed);
    learning_.store(false, std::memory_order_relaxed);
    profileReady_.store(false, std::memory_order_relaxed);
    currentReductionDb_.store(0.0f, std::memory_order_relaxed);
    if (!noise_.empty() && !effect_.hasProfile()) {
        std::fill(noise_.begin(), noise_.end(), 0.0f);
        noiseSeeded_ = false;
    }
}

void SpectralDenoiserProcessor::startLearning() noexcept {
    learnRequest_.store(true, std::memory_order_release);
}

float SpectralDenoiserProcessor::learnProgress() const noexcept {
    if (profileReady_.load(std::memory_order_acquire)) {
        return 1.0f;
    }
    if (!learning_.load(std::memory_order_acquire)) {
        return 0.0f;
    }
    const int target = learnFrameTarget(sampleRate_);
    return std::clamp(static_cast<float>(learnFrames_) / static_cast<float>(target), 0.0f, 1.0f);
}

bool SpectralDenoiserProcessor::takeLearnedProfile(std::vector<float>& profile, double& sampleRate) noexcept {
    if (!profileReady_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    profile = pendingProfile_;
    sampleRate = sampleRate_;
    return true;
}

void SpectralDenoiserProcessor::process(float* left, float* right, int frames) noexcept {
    if (!prepared_ || left == nullptr || right == nullptr || frames <= 0) {
        return;
    }

    constexpr std::size_t fftSize = static_cast<std::size_t>(kSpectralDenoiserFftSize);

    for (int i = 0; i < frames; ++i) {
        float l = left[i];
        float r = right[i];
        if (!std::isfinite(l)) {
            l = 0.0f;
        }
        if (!std::isfinite(r)) {
            r = 0.0f;
        }

        inputLeft_[inputWrite_] = l;
        inputRight_[inputWrite_] = r;
        inputWrite_ = (inputWrite_ + 1) & (fftSize - 1);
        ++inputSamples_;

        if (++hopCounter_ >= kSpectralDenoiserHopSize) {
            hopCounter_ = 0;
            if (inputSamples_ >= fftSize) {
                processFrame();
            }
        }

        if (inputSamples_ > fftSize) {
            const std::uint64_t position = inputSamples_ - fftSize - 1;
            const auto slot = static_cast<std::size_t>(position & kOverlapMask);
            left[i] = overlapLeft_[slot];
            right[i] = overlapRight_[slot];
            overlapLeft_[slot] = 0.0f;
            overlapRight_[slot] = 0.0f;
        } else {
            left[i] = 0.0f;
            right[i] = 0.0f;
        }
    }
}

void SpectralDenoiserProcessor::processFrame() noexcept {
    const auto bins = static_cast<std::size_t>(kSpectralDenoiserBinCount);
    const auto fftSize = static_cast<std::size_t>(kSpectralDenoiserFftSize);
    const std::uint64_t frameStart = inputSamples_ - fftSize;
    const std::size_t oldest = inputWrite_;

    for (std::size_t n = 0; n < fftSize; ++n) {
        const std::size_t source = (oldest + n) & (fftSize - 1);
        const float w = window_[n];
        reLeft_[n] = inputLeft_[source] * w;
        imLeft_[n] = 0.0f;
        reRight_[n] = inputRight_[source] * w;
        imRight_[n] = 0.0f;
    }
    fft(reLeft_, imLeft_, false);
    fft(reRight_, imRight_, false);

    if (learnRequest_.exchange(false, std::memory_order_acq_rel)) {
        std::fill(learnSum_.begin(), learnSum_.end(), 0.0);
        learnFrames_ = 0;
        profileReady_.store(false, std::memory_order_relaxed);
        learning_.store(true, std::memory_order_release);
    }

    const bool bypassed = effect_.bypassed;
    const bool learning = learning_.load(std::memory_order_acquire);
    const bool useStaticProfile = effect_.mode == SpectralDenoiserMode::Learn && effect_.hasProfile();
    const bool adaptive = !useStaticProfile;
    const float floorGain = dsp::dbToGain(-clampReduction(effect_.reductionDb));

    for (std::size_t k = 0; k < bins; ++k) {
        const float powerLeft = reLeft_[k] * reLeft_[k] + imLeft_[k] * imLeft_[k];
        const float powerRight = reRight_[k] * reRight_[k] + imRight_[k] * imRight_[k];
        const float power = std::max(0.5f * (powerLeft + powerRight), 0.0f);

        if (learning) {
            learnSum_[k] += static_cast<double>(power);
            rawGain_[k] = 1.0f;
            continue;
        }

        if (bypassed || (effect_.mode == SpectralDenoiserMode::Learn && !effect_.hasProfile())) {
            rawGain_[k] = 1.0f;
            continue;
        }

        if (adaptive) {
            if (!noiseSeeded_) {
                noise_[k] = std::max(power, kSilencePower);
                noiseSeeded_ = true;
            } else if (power < noise_[k]) {
                // Track falling noise quickly, rising program material slowly.
                noise_[k] += 0.02f * (power - noise_[k]);
            } else {
                noise_[k] += 0.0005f * (power - noise_[k]);
            }
        }

        if (power <= kSilencePower) {
            rawGain_[k] = 1.0f;
            continue;
        }

        const float noisePower = std::max(noise_[k], kSilencePower);
        const float snr = std::max(0.0f, power / noisePower - 1.0f);
        const float wiener = snr / (snr + 1.0f);
        rawGain_[k] = floorGain + (1.0f - floorGain) * wiener;
    }

    if (learning) {
        learnFrames_ += 1;
        if (learnFrames_ >= learnFrameTarget(sampleRate_)) {
            const float scale = 1.0f / static_cast<float>(learnFrames_);
            for (std::size_t k = 0; k < bins; ++k) {
                pendingProfile_[k] = static_cast<float>(learnSum_[k]) * scale;
            }
            learning_.store(false, std::memory_order_release);
            profileReady_.store(true, std::memory_order_release);
        }
    }

    // Smooth across neighbouring bins, then in time, to avoid musical noise.
    if (bins > 2) {
        frequencyGain_[0] = rawGain_[0];
        for (std::size_t k = 1; k + 1 < bins; ++k) {
            frequencyGain_[k] = 0.25f * rawGain_[k - 1] + 0.5f * rawGain_[k]
                + 0.25f * rawGain_[k + 1];
        }
        frequencyGain_[bins - 1] = rawGain_[bins - 1];
    } else {
        frequencyGain_ = rawGain_;
    }

    float minimumGain = 1.0f;
    for (std::size_t k = 0; k < bins; ++k) {
        const float target = std::clamp(frequencyGain_[k], 0.0f, 1.0f);
        const float coeff = target < smoothGain_[k] ? attackCoeff_ : releaseCoeff_;
        smoothGain_[k] += coeff * (target - smoothGain_[k]);
        minimumGain = std::min(minimumGain, smoothGain_[k]);
    }
    currentReductionDb_.store(-dsp::gainToDb(minimumGain), std::memory_order_relaxed);

    for (std::size_t k = 0; k < bins; ++k) {
        reLeft_[k] *= smoothGain_[k];
        imLeft_[k] *= smoothGain_[k];
        reRight_[k] *= smoothGain_[k];
        imRight_[k] *= smoothGain_[k];
        if (k == 0 || k + 1 == bins) {
            continue;
        }
        // Mirror the mask onto the negative frequencies so the inverse
        // transform stays real.
        const std::size_t mirror = static_cast<std::size_t>(kSpectralDenoiserFftSize) - k;
        reLeft_[mirror] *= smoothGain_[k];
        imLeft_[mirror] *= smoothGain_[k];
        reRight_[mirror] *= smoothGain_[k];
        imRight_[mirror] *= smoothGain_[k];
    }
    fft(reLeft_, imLeft_, true);
    fft(reRight_, imRight_, true);

    for (std::size_t n = 0; n < fftSize; ++n) {
        const float w = window_[n] * overlapNorm_;
        const auto slot = static_cast<std::size_t>((frameStart + n) & kOverlapMask);
        overlapLeft_[slot] += reLeft_[n] * w;
        overlapRight_[slot] += reRight_[n] * w;
    }
}

void SpectralDenoiserProcessor::fft(std::vector<float>& re, std::vector<float>& im, bool inverse) const noexcept {
    const int n = kSpectralDenoiserFftSize;

    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[static_cast<std::size_t>(i)], re[static_cast<std::size_t>(j)]);
            std::swap(im[static_cast<std::size_t>(i)], im[static_cast<std::size_t>(j)]);
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * dsp::kPi / static_cast<double>(len);
        const float stepRe = static_cast<float>(std::cos(angle));
        const float stepIm = static_cast<float>(std::sin(angle));
        const int half = len >> 1;
        for (int start = 0; start < n; start += len) {
            float twiddleRe = 1.0f;
            float twiddleIm = 0.0f;
            for (int k = 0; k < half; ++k) {
                const auto a = static_cast<std::size_t>(start + k);
                const auto b = static_cast<std::size_t>(start + k + half);
                const float vr = re[b] * twiddleRe - im[b] * twiddleIm;
                const float vi = re[b] * twiddleIm + im[b] * twiddleRe;
                const float ur = re[a];
                const float ui = im[a];
                re[a] = ur + vr;
                im[a] = ui + vi;
                re[b] = ur - vr;
                im[b] = ui - vi;
                const float nextRe = twiddleRe * stepRe - twiddleIm * stepIm;
                twiddleIm = twiddleRe * stepIm + twiddleIm * stepRe;
                twiddleRe = nextRe;
            }
        }
    }

    if (inverse) {
        const float scale = 1.0f / static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            re[static_cast<std::size_t>(i)] *= scale;
            im[static_cast<std::size_t>(i)] *= scale;
        }
    }
}

} // namespace pluginrack
