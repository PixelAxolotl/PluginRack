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

int learnFrameTarget(double sampleRate, float seconds) noexcept {
    const double length = std::clamp(static_cast<double>(seconds),
                                     static_cast<double>(kSpectralDenoiserMinLearnSeconds),
                                     static_cast<double>(kSpectralDenoiserMaxLearnSeconds));
    const auto frames = static_cast<int>(
        std::lround(sampleRate * length / kSpectralDenoiserHopSize));
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
    trimGain_ = other.trimGain_;
    rawGain_ = other.rawGain_;
    frequencyGain_ = other.frequencyGain_;
    smoothGain_ = other.smoothGain_;
    rawGainLeft_ = other.rawGainLeft_;
    rawGainRight_ = other.rawGainRight_;
    frequencyGainLeft_ = other.frequencyGainLeft_;
    frequencyGainRight_ = other.frequencyGainRight_;
    smoothGainLeft_ = other.smoothGainLeft_;
    smoothGainRight_ = other.smoothGainRight_;
    learnSum_ = other.learnSum_;
    pendingProfile_ = other.pendingProfile_;
    inputWrite_ = other.inputWrite_;
    inputSamples_ = other.inputSamples_;
    hopCounter_ = other.hopCounter_;
    noiseSeeded_ = other.noiseSeeded_;
    learnFrames_.store(other.learnFrames_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    learnTargetFrames_ = other.learnTargetFrames_;
    overlapNorm_ = other.overlapNorm_;
    attackCoeff_ = other.attackCoeff_;
    releaseCoeff_ = other.releaseCoeff_;
    learnRequest_.store(other.learnRequest_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    learning_.store(other.learning_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    profileReady_.store(other.profileReady_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    currentReductionDb_.store(other.currentReductionDb_.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
    for (std::size_t band = 0; band < bandReductionDb_.size(); ++band) {
        bandReductionDb_[band].store(
            other.bandReductionDb_[band].load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }
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
    trimGain_.assign(bins, 1.0f);
    rawGain_.assign(bins, 1.0f);
    frequencyGain_.assign(bins, 1.0f);
    smoothGain_.assign(bins, 1.0f);
    rawGainLeft_.assign(bins, 1.0f);
    rawGainRight_.assign(bins, 1.0f);
    frequencyGainLeft_.assign(bins, 1.0f);
    frequencyGainRight_.assign(bins, 1.0f);
    smoothGainLeft_.assign(bins, 1.0f);
    smoothGainRight_.assign(bins, 1.0f);
    learnSum_.assign(bins, 0.0);
    pendingProfile_.assign(bins, 0.0f);

    learnTargetFrames_ = learnFrameTarget(sampleRate_, effect_.learnSeconds);
    updateSmoothing();

    prepared_ = true;
    rebuildNoiseFromProfile();
    rebuildTrimGains();
    reset();
}

void SpectralDenoiserProcessor::setEffect(const SpectralDenoiserEffect& effect) {
    effect_ = effect;
    if (prepared_) {
        rebuildNoiseFromProfile();
        rebuildTrimGains();
        learnTargetFrames_ = learnFrameTarget(sampleRate_, effect_.learnSeconds);
        updateSmoothing();
    }
}

void SpectralDenoiserProcessor::updateSmoothing() {
    // Frames arrive every hop; express the time constants in frames. Speed
    // scales both ends so one control can go from gentle to snappy.
    const auto frameRate = static_cast<double>(sampleRate_) / kSpectralDenoiserHopSize;
    const float speed = std::clamp(effect_.speed, kSpectralDenoiserMinSpeed,
                                   kSpectralDenoiserMaxSpeed);
    attackCoeff_ = dsp::smoothingCoeff(frameRate, 10.0f / speed);
    releaseCoeff_ = dsp::smoothingCoeff(frameRate, 60.0f / speed);
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

void SpectralDenoiserProcessor::rebuildTrimGains() {
    const auto bins = static_cast<std::size_t>(kSpectralDenoiserBinCount);
    trimGain_.assign(bins, 1.0f);
    if (effect_.profileTrimDb.size()
        != static_cast<std::size_t>(kSpectralDenoiserTrimBandCount)) {
        return;
    }
    const double nyquist = sampleRate_ * 0.5;
    for (std::size_t k = 0; k < bins; ++k) {
        const double frequency = static_cast<double>(k) * sampleRate_
            / static_cast<double>(kSpectralDenoiserFftSize);
        trimGain_[k] = dsp::dbToGain(
            spectralDenoiserTrimDbAt(effect_.profileTrimDb, frequency, nyquist));
    }
}

void SpectralDenoiserProcessor::reset() noexcept {
    std::fill(inputLeft_.begin(), inputLeft_.end(), 0.0f);
    std::fill(inputRight_.begin(), inputRight_.end(), 0.0f);
    std::fill(overlapLeft_.begin(), overlapLeft_.end(), 0.0f);
    std::fill(overlapRight_.begin(), overlapRight_.end(), 0.0f);
    std::fill(smoothGain_.begin(), smoothGain_.end(), 1.0f);
    std::fill(rawGain_.begin(), rawGain_.end(), 1.0f);
    std::fill(frequencyGain_.begin(), frequencyGain_.end(), 1.0f);
    std::fill(smoothGainLeft_.begin(), smoothGainLeft_.end(), 1.0f);
    std::fill(smoothGainRight_.begin(), smoothGainRight_.end(), 1.0f);
    std::fill(rawGainLeft_.begin(), rawGainLeft_.end(), 1.0f);
    std::fill(rawGainRight_.begin(), rawGainRight_.end(), 1.0f);
    std::fill(frequencyGainLeft_.begin(), frequencyGainLeft_.end(), 1.0f);
    std::fill(frequencyGainRight_.begin(), frequencyGainRight_.end(), 1.0f);
    std::fill(learnSum_.begin(), learnSum_.end(), 0.0);
    inputWrite_ = 0;
    inputSamples_ = 0;
    hopCounter_ = 0;
    learnFrames_ = 0;
    learnRequest_.store(false, std::memory_order_relaxed);
    learning_.store(false, std::memory_order_relaxed);
    profileReady_.store(false, std::memory_order_relaxed);
    currentReductionDb_.store(0.0f, std::memory_order_relaxed);
    for (auto& band : bandReductionDb_) {
        band.store(0.0f, std::memory_order_relaxed);
    }
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
    return std::clamp(static_cast<float>(learnFrames_)
                          / static_cast<float>(std::max(learnTargetFrames_, 1)),
                      0.0f, 1.0f);
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
    const float mix = std::clamp(effect_.mixPercent, kSpectralDenoiserMinMixPercent,
                                 kSpectralDenoiserMaxMixPercent)
        * 0.01f;
    const bool listen = effect_.listen && !effect_.bypassed;

    for (int i = 0; i < frames; ++i) {
        float l = left[i];
        float r = right[i];
        if (!std::isfinite(l)) {
            l = 0.0f;
        }
        if (!std::isfinite(r)) {
            r = 0.0f;
        }

        // The slot about to be overwritten still holds the input from one frame
        // ago, which is exactly the dry signal at the reported latency.
        const float dryLeft = inputLeft_[inputWrite_];
        const float dryRight = inputRight_[inputWrite_];

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
            const float wetLeft = overlapLeft_[slot];
            const float wetRight = overlapRight_[slot];
            overlapLeft_[slot] = 0.0f;
            overlapRight_[slot] = 0.0f;
            if (listen) {
                left[i] = dryLeft - wetLeft;
                right[i] = dryRight - wetRight;
            } else if (mix >= 0.999f) {
                left[i] = wetLeft;
                right[i] = wetRight;
            } else {
                left[i] = dryLeft + (wetLeft - dryLeft) * mix;
                right[i] = dryRight + (wetRight - dryRight) * mix;
            }
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
    const float thresholdGain = dsp::dbToGain(
        std::clamp(effect_.thresholdOffsetDb, kSpectralDenoiserMinThresholdOffsetDb,
                   kSpectralDenoiserMaxThresholdOffsetDb));
    const float linkAmount = std::clamp(effect_.stereoLinkPercent,
                                        kSpectralDenoiserMinStereoLinkPercent,
                                        kSpectralDenoiserMaxStereoLinkPercent)
        * 0.01f;

    for (std::size_t k = 0; k < bins; ++k) {
        const float powerLeft = reLeft_[k] * reLeft_[k] + imLeft_[k] * imLeft_[k];
        const float powerRight = reRight_[k] * reRight_[k] + imRight_[k] * imRight_[k];
        const float power = std::max(0.5f * (powerLeft + powerRight), 0.0f);

        if (learning) {
            learnSum_[k] += static_cast<double>(power);
            rawGain_[k] = 1.0f;
            rawGainLeft_[k] = 1.0f;
            rawGainRight_[k] = 1.0f;
            continue;
        }

        if (bypassed || (effect_.mode == SpectralDenoiserMode::Learn && !effect_.hasProfile())) {
            rawGain_[k] = 1.0f;
            rawGainLeft_[k] = 1.0f;
            rawGainRight_[k] = 1.0f;
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
            rawGainLeft_[k] = 1.0f;
            rawGainRight_[k] = 1.0f;
            continue;
        }

        const float noisePower = std::max(noise_[k], kSilencePower) * trimGain_[k]
            * thresholdGain;
        const auto gainForPower = [noisePower, floorGain](float binPower) {
            const float snr = std::max(0.0f, binPower / noisePower - 1.0f);
            const float wiener = snr / (snr + 1.0f);
            return floorGain + (1.0f - floorGain) * wiener;
        };
        const float linkedGain = gainForPower(power);
        rawGain_[k] = linkedGain;
        // The noise estimate stays shared; below 100% each side decides how
        // much of that estimate to act on using its own spectrum.
        rawGainLeft_[k] = linkAmount * linkedGain
            + (1.0f - linkAmount) * gainForPower(powerLeft);
        rawGainRight_[k] = linkAmount * linkedGain
            + (1.0f - linkAmount) * gainForPower(powerRight);
    }

    if (learning) {
        learnFrames_ += 1;
        if (learnFrames_ >= std::max(learnTargetFrames_, 1)) {
            const float scale = 1.0f / static_cast<float>(learnFrames_);
            for (std::size_t k = 0; k < bins; ++k) {
                pendingProfile_[k] = static_cast<float>(learnSum_[k]) * scale;
            }
            learning_.store(false, std::memory_order_release);
            profileReady_.store(true, std::memory_order_release);
        }
    }

    // Smooth across neighbouring bins, then in time, to avoid musical noise.
    const auto smoothMask = [this, bins](const std::vector<float>& raw,
                                         std::vector<float>& frequency,
                                         std::vector<float>& smooth) {
        if (bins > 2) {
            frequency[0] = raw[0];
            for (std::size_t k = 1; k + 1 < bins; ++k) {
                frequency[k] = 0.25f * raw[k - 1] + 0.5f * raw[k] + 0.25f * raw[k + 1];
            }
            frequency[bins - 1] = raw[bins - 1];
        } else {
            frequency = raw;
        }

        float minimum = 1.0f;
        for (std::size_t k = 0; k < bins; ++k) {
            const float target = std::clamp(frequency[k], 0.0f, 1.0f);
            const float coeff = target < smooth[k] ? attackCoeff_ : releaseCoeff_;
            smooth[k] += coeff * (target - smooth[k]);
            minimum = std::min(minimum, smooth[k]);
        }
        return minimum;
    };

    const float minimumGain = smoothMask(rawGain_, frequencyGain_, smoothGain_);
    currentReductionDb_.store(-dsp::gainToDb(minimumGain), std::memory_order_relaxed);

    const bool useSideMasks = linkAmount < 0.999f;
    if (useSideMasks) {
        smoothMask(rawGainLeft_, frequencyGainLeft_, smoothGainLeft_);
        smoothMask(rawGainRight_, frequencyGainRight_, smoothGainRight_);
    }

    // Read the meter at each band centre. Log bands below the FFT resolution
    // contain no bin of their own; sampling the gain curve keeps every bar
    // populated instead of leaving holes.
    const double binWidth = sampleRate_ / kSpectralDenoiserFftSize;
    const double nyquist = sampleRate_ * 0.5;
    const double bandRatio = nyquist / kSpectralDenoiserTrimLowHz;
    for (std::size_t band = 0; band < bandReductionDb_.size(); ++band) {
        const double centre = kSpectralDenoiserTrimLowHz
            * std::pow(bandRatio,
                       (static_cast<double>(band) + 0.5)
                           / kSpectralDenoiserTrimBandCount);
        double position = std::clamp(centre / binWidth, 1.0,
                                     static_cast<double>(bins - 1));
        const auto index = static_cast<std::size_t>(position);
        const auto next = std::min(index + 1, bins - 1);
        const auto fraction = static_cast<float>(position - static_cast<double>(index));
        const float gain = smoothGain_[index]
            + (smoothGain_[next] - smoothGain_[index]) * fraction;
        const float reduction = std::clamp(-dsp::gainToDb(gain),
                                           kSpectralDenoiserMinReductionDb,
                                           kSpectralDenoiserMaxReductionDb);
        bandReductionDb_[band].store(reduction, std::memory_order_relaxed);
    }

    for (std::size_t k = 0; k < bins; ++k) {
        const float maskLeft = useSideMasks ? smoothGainLeft_[k] : smoothGain_[k];
        const float maskRight = useSideMasks ? smoothGainRight_[k] : smoothGain_[k];
        reLeft_[k] *= maskLeft;
        imLeft_[k] *= maskLeft;
        reRight_[k] *= maskRight;
        imRight_[k] *= maskRight;
        if (k == 0 || k + 1 == bins) {
            continue;
        }
        // Mirror the mask onto the negative frequencies so the inverse
        // transform stays real.
        const std::size_t mirror = static_cast<std::size_t>(kSpectralDenoiserFftSize) - k;
        reLeft_[mirror] *= maskLeft;
        imLeft_[mirror] *= maskLeft;
        reRight_[mirror] *= maskRight;
        imRight_[mirror] *= maskRight;
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
