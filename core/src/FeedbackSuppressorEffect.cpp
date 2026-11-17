#include "pluginrack/FeedbackSuppressorEffect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

namespace pluginrack {

namespace {
constexpr float kPi = 3.14159265358979323846f;

void fft(std::vector<float>& re, std::vector<float>& im) {
    const int n = static_cast<int>(re.size());
    // bit reversal
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * kPi / static_cast<float>(len);
        const float wlenRe = std::cos(ang);
        const float wlenIm = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float wRe = 1.0f, wIm = 0.0f;
            for (int j = 0; j < len / 2; ++j) {
                const float uRe = re[i + j];
                const float uIm = im[i + j];
                const float vRe = re[i + j + len / 2] * wRe - im[i + j + len / 2] * wIm;
                const float vIm = re[i + j + len / 2] * wIm + im[i + j + len / 2] * wRe;
                re[i + j] = uRe + vRe;
                im[i + j] = uIm + vIm;
                re[i + j + len / 2] = uRe - vRe;
                im[i + j + len / 2] = uIm - vIm;
                const float nwRe = wRe * wlenRe - wIm * wlenIm;
                const float nwIm = wRe * wlenIm + wIm * wlenRe;
                wRe = nwRe;
                wIm = nwIm;
            }
        }
    }
}

// Keep at least a small cut for real feedback, but let a strong ringing tone
// grow toward the Attenuation knob instead of always cutting at full depth.
float dynamicDepthDb(float maxDepthDb, float peakDbfs, float gateDb,
                     float multiplier) noexcept {
    const float depth = std::clamp(maxDepthDb, kFeedbackSuppressorMinAttenuationDb,
                                   kFeedbackSuppressorMaxAttenuationDb);
    if (depth <= 0.0f) return 0.0f;
    const float minDepth = std::min(depth, std::max(3.0f, depth * 0.25f));
    const float span = std::max(6.0f, -gateDb);
    const float t = std::clamp((peakDbfs - gateDb) / span, 0.0f, 1.0f);
    const float scaled = (minDepth + t * (depth - minDepth)) * multiplier;
    return std::clamp(scaled, 0.0f, depth);
}
} // namespace

FeedbackSuppressorProcessor::FeedbackSuppressorProcessor(
    const FeedbackSuppressorProcessor& other) {
    *this = other;
}

FeedbackSuppressorProcessor& FeedbackSuppressorProcessor::operator=(
    const FeedbackSuppressorProcessor& other) {
    if (this == &other) return *this;
    sampleRate_ = other.sampleRate_;
    effect_ = other.effect_;
    notches_ = other.notches_;
    candidates_ = other.candidates_;
    attackStep_ = other.attackStep_;
    releaseStep_ = other.releaseStep_;
    holdSamples_ = other.holdSamples_;
    hpB0_ = other.hpB0_;
    hpB1_ = other.hpB1_;
    hpB2_ = other.hpB2_;
    hpA1_ = other.hpA1_;
    hpA2_ = other.hpA2_;
    hpZ1_ = other.hpZ1_;
    hpZ2_ = other.hpZ2_;
    lpB0_ = other.lpB0_;
    lpB1_ = other.lpB1_;
    lpB2_ = other.lpB2_;
    lpA1_ = other.lpA1_;
    lpA2_ = other.lpA2_;
    lpZ1_ = other.lpZ1_;
    lpZ2_ = other.lpZ2_;
    for (std::size_t i = 0; i < publishedFreqs_.size(); ++i)
        publishedFreqs_[i].store(
            other.publishedFreqs_[i].load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    for (std::size_t i = 0; i < publishedDepths_.size(); ++i)
        publishedDepths_[i].store(
            other.publishedDepths_[i].load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    publishedCount_.store(other.publishedCount_.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    analysisBuffer_ = other.analysisBuffer_;
    analysisWrite_ = other.analysisWrite_;
    analysisCount_ = other.analysisCount_;
    framesSinceAnalysis_ = other.framesSinceAnalysis_;
    fftReal_ = other.fftReal_;
    fftImag_ = other.fftImag_;
    mags_ = other.mags_;
    fftLowReal_ = other.fftLowReal_;
    fftLowImag_ = other.fftLowImag_;
    magsLow_ = other.magsLow_;
    peaks_ = other.peaks_;
    detections_ = other.detections_;
    analysisPass_ = other.analysisPass_;
    reductionDb_ = other.reductionDb_;
    inputRmsAccum_ = other.inputRmsAccum_;
    outputRmsAccum_ = other.outputRmsAccum_;
    rmsCount_ = other.rmsCount_;
    sensitivityLinear_ = other.sensitivityLinear_;
    gateOpen_ = other.gateOpen_;
    return *this;
}

FeedbackSuppressorEffect makeDefaultFeedbackSuppressorEffect(std::uint32_t id) {
    FeedbackSuppressorEffect effect;
    effect.id = id;
    effect.bypassed = false;
    effect.strength = feedbackSuppressorStrengthForCuts(kFeedbackSuppressorDefaultCuts);
    effect.attenuationDb = kFeedbackSuppressorDefaultAttenuationDb;
    effect.gainReductionMultiplier = kFeedbackSuppressorDefaultGainReductionMultiplier;
    effect.attackMs = kFeedbackSuppressorDefaultAttackMs;
    effect.holdMs = kFeedbackSuppressorDefaultHoldMs;
    effect.releaseMs = kFeedbackSuppressorDefaultReleaseMs;
    effect.detectionDelayMs = kFeedbackSuppressorDefaultDetectionDelayMs;
    effect.inputSensitivityDb = kFeedbackSuppressorDefaultSensitivityDb;
    effect.detectorHighPassHz = kFeedbackSuppressorDefaultHighPassHz;
    effect.detectorLowPassHz = kFeedbackSuppressorDefaultLowPassHz;
    return effect;
}

std::vector<FeedbackSuppressorEffect> defaultFeedbackSuppressorEffects() {
    return {makeDefaultFeedbackSuppressorEffect(1)};
}

float feedbackSuppressorStaticGainDb(float /*levelDb*/, const FeedbackSuppressorEffect& /*effect*/) {
    return 0.0f;
}

void FeedbackSuppressorProcessor::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    analysisBuffer_.assign(kLowAnalysisSize, 0.0f);
    fftReal_.assign(kHighAnalysisSize, 0.0f);
    fftImag_.assign(kHighAnalysisSize, 0.0f);
    mags_.assign(kHighAnalysisSize / 2, 0.0f);
    fftLowReal_.assign(kLowAnalysisSize, 0.0f);
    fftLowImag_.assign(kLowAnalysisSize, 0.0f);
    magsLow_.assign(kLowAnalysisSize / 2, 0.0f);
    peaks_.clear();
    peaks_.reserve(kLowAnalysisSize / 2);
    detections_.clear();
    detections_.reserve(kMaxNotches);
    analysisWrite_ = 0;
    analysisCount_ = 0;
    framesSinceAnalysis_ = 0;
    reductionDb_ = 0.0f;
    inputRmsAccum_ = 0.0f;
    outputRmsAccum_ = 0.0f;
    rmsCount_ = 0;
    gateOpen_ = false;
    refreshEnvelopeSteps();
    for (auto& n : notches_) n = Notch{};
    clearCandidates();
    for (auto& f : publishedFreqs_) f.store(0.0f, std::memory_order_relaxed);
    for (auto& d : publishedDepths_) d.store(0.0f, std::memory_order_relaxed);
    publishedCount_.store(0, std::memory_order_relaxed);
    setEffect(effect_);
}

int FeedbackSuppressorProcessor::copyActiveNotches(float* freqs, float* depthsDb,
                                                  int max) const noexcept {
    if (max <= 0) return 0;
    const int count = std::min(max, publishedCount_.load(std::memory_order_relaxed));
    for (int i = 0; i < count; ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (freqs != nullptr)
            freqs[i] = publishedFreqs_[index].load(std::memory_order_relaxed);
        if (depthsDb != nullptr)
            depthsDb[i] = publishedDepths_[index].load(std::memory_order_relaxed);
    }
    return count;
}

int FeedbackSuppressorProcessor::copyActiveNotches(float* dest, int max) const noexcept {
    if (dest == nullptr) return 0;
    return copyActiveNotches(dest, nullptr, max);
}

void FeedbackSuppressorProcessor::setEffect(const FeedbackSuppressorEffect& effect) {
    effect_ = effect;
    effect_.strength = std::clamp(effect_.strength, 0.0f, 1.0f);
    effect_.attenuationDb = std::clamp(effect_.attenuationDb,
                                       kFeedbackSuppressorMinAttenuationDb,
                                       kFeedbackSuppressorMaxAttenuationDb);
    effect_.gainReductionMultiplier = std::clamp(
        effect_.gainReductionMultiplier,
        kFeedbackSuppressorMinGainReductionMultiplier,
        kFeedbackSuppressorMaxGainReductionMultiplier);
    effect_.attackMs = std::clamp(effect_.attackMs,
                                  kFeedbackSuppressorMinAttackMs,
                                  kFeedbackSuppressorMaxAttackMs);
    effect_.holdMs = std::clamp(effect_.holdMs,
                                kFeedbackSuppressorMinHoldMs,
                                kFeedbackSuppressorMaxHoldMs);
    effect_.releaseMs = std::clamp(effect_.releaseMs,
                                   kFeedbackSuppressorMinReleaseMs,
                                   kFeedbackSuppressorMaxReleaseMs);
    effect_.detectionDelayMs = std::clamp(effect_.detectionDelayMs,
                                          kFeedbackSuppressorMinDetectionDelayMs,
                                          kFeedbackSuppressorMaxDetectionDelayMs);
    effect_.inputSensitivityDb = std::clamp(effect_.inputSensitivityDb,
                                            kFeedbackSuppressorMinSensitivityDb,
                                            kFeedbackSuppressorMaxSensitivityDb);
    const float highPass = std::clamp(effect_.detectorHighPassHz,
                                      kFeedbackSuppressorMinDetectorHz,
                                      kFeedbackSuppressorMaxHighPassHz);
    effect_.detectorHighPassHz = highPass;
    effect_.detectorLowPassHz = std::clamp(effect_.detectorLowPassHz,
                                           std::max(kFeedbackSuppressorMinLowPassHz,
                                                    highPass + kFeedbackSuppressorDetectorGapHz),
                                           kFeedbackSuppressorMaxDetectorHz);
    sensitivityLinear_ = std::pow(10.0f, effect_.inputSensitivityDb / 20.0f);
    updateAnalysisHighPass();
    updateAnalysisLowPass();
    refreshEnvelopeSteps();
    // Attenuation changes the peaking-filter coefficients. Refresh any cut
    // that is currently alive so the knob responds without waiting for the
    // next detector pass.
    for (auto& notch : notches_) {
        if (notch.active) updateNotchCoefficients(notch);
    }
}

void FeedbackSuppressorProcessor::refreshEnvelopeSteps() noexcept {
    const float sampleRate = static_cast<float>(std::max(1.0, sampleRate_));
    attackStep_ = effect_.attackMs <= 0.0f
        ? 1.0f
        : 1.0f / std::max(1.0f, effect_.attackMs * 0.001f * sampleRate);
    releaseStep_ = 1.0f / std::max(1.0f, effect_.releaseMs * 0.001f * sampleRate);
    holdSamples_ = std::max(0, static_cast<int>(std::lround(effect_.holdMs * 0.001f * sampleRate)));
}

void FeedbackSuppressorProcessor::updateNotchCoefficients(Notch& notch) const noexcept {
    const float attenuation = std::clamp(notch.depthDb,
                                         kFeedbackSuppressorMinAttenuationDb,
                                         kFeedbackSuppressorMaxAttenuationDb);
    const float a = std::pow(10.0f, -attenuation / 40.0f);
    const float binWidth = notch.binWidthHz > 0.0f
        ? notch.binWidthHz
        : static_cast<float>(sampleRate_) / static_cast<float>(kHighAnalysisSize);
    const float q = feedbackSuppressorNotchQ(attenuation, notch.freq, binWidth);
    const float w0 = 2.0f * kPi * notch.freq / static_cast<float>(sampleRate_);
    const float alpha = std::sin(w0) / (2.0f * q);
    const float cosw = std::cos(w0);
    const float a0 = 1.0f + alpha / a;
    notch.b0 = (1.0f + alpha * a) / a0;
    notch.b1 = (-2.0f * cosw) / a0;
    notch.b2 = (1.0f - alpha * a) / a0;
    notch.a1 = (-2.0f * cosw) / a0;
    notch.a2 = (1.0f - alpha / a) / a0;
}

void FeedbackSuppressorProcessor::reset() noexcept {
    closeGate();
}

void FeedbackSuppressorProcessor::clearAnalysisHistory() noexcept {
    analysisWrite_ = 0;
    analysisCount_ = 0;
    framesSinceAnalysis_ = 0;
    std::fill(analysisBuffer_.begin(), analysisBuffer_.end(), 0.0f);
    inputRmsAccum_ = 0.0f;
    outputRmsAccum_ = 0.0f;
    rmsCount_ = 0;
    reductionDb_ = 0.0f;
    clearCandidates();
    resetAnalysisHighPass();
    resetAnalysisLowPass();
}

void FeedbackSuppressorProcessor::clearCandidates() noexcept {
    for (auto& candidate : candidates_) candidate = Candidate{};
}

void FeedbackSuppressorProcessor::resetAnalysisHighPass() noexcept {
    hpZ1_.fill(0.0f);
    hpZ2_.fill(0.0f);
}

void FeedbackSuppressorProcessor::resetAnalysisLowPass() noexcept {
    lpZ1_.fill(0.0f);
    lpZ2_.fill(0.0f);
}

void FeedbackSuppressorProcessor::updateAnalysisLowPass() noexcept {
    // Four cascaded RBJ low-pass sections form an 8th-order (48 dB/oct)
    // Butterworth filter. The FFT search already ignores bins above the LPF,
    // but a loud out-of-band tone still leaks into in-band bins through the
    // window; the steeper filter keeps that leakage out of the detector.
    const double fs = std::max(1.0, sampleRate_);
    const double nyquist = fs * 0.5;
    const double f = std::clamp(static_cast<double>(effect_.detectorLowPassHz),
                                40.0,
                                std::min(20000.0, nyquist * 0.95));
    const double w0 = 2.0 * 3.14159265358979323846 * f / fs;
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);
    constexpr std::array<double, 4> kButterworthQ{
        0.5097955791041592, 0.6013448869350453,
        0.8999762231364156, 2.5629154477415055};
    bool valid = true;
    for (int stage = 0; stage < 4; ++stage) {
        const double alpha = sinw / (2.0 * kButterworthQ[static_cast<std::size_t>(stage)]);
        const double a0 = 1.0 + alpha;
        if (std::fabs(a0) < 1e-12) {
            valid = false;
            break;
        }
        lpB0_[static_cast<std::size_t>(stage)] =
            static_cast<float>(((1.0 - cosw) * 0.5) / a0);
        lpB1_[static_cast<std::size_t>(stage)] =
            static_cast<float>((1.0 - cosw) / a0);
        lpB2_[static_cast<std::size_t>(stage)] =
            lpB0_[static_cast<std::size_t>(stage)];
        lpA1_[static_cast<std::size_t>(stage)] =
            static_cast<float>((-2.0 * cosw) / a0);
        lpA2_[static_cast<std::size_t>(stage)] =
            static_cast<float>((1.0 - alpha) / a0);
    }
    if (!valid) {
        lpB0_.fill(0.0f);
        lpB1_.fill(0.0f);
        lpB2_.fill(0.0f);
        lpA1_.fill(0.0f);
        lpA2_.fill(0.0f);
        lpB0_[0] = 1.0f;
    }
    resetAnalysisLowPass();
}

void FeedbackSuppressorProcessor::updateAnalysisHighPass() noexcept {
    // Four cascaded RBJ high-pass sections form an 8th-order (48 dB/oct)
    // Butterworth filter on the mono analysis signal. This keeps DC, sub-bass
    // rumble and loud below-band content out of the detector without touching
    // the audio output.
    const double fs = std::max(1.0, sampleRate_);
    const double f = std::clamp(static_cast<double>(effect_.detectorHighPassHz),
                                10.0,
                                std::min(20000.0, fs * 0.45));
    const double w0 = 2.0 * 3.14159265358979323846 * f / fs;
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);
    constexpr std::array<double, 4> kButterworthQ{
        0.5097955791041592, 0.6013448869350453,
        0.8999762231364156, 2.5629154477415055};
    bool valid = true;
    for (int stage = 0; stage < 4; ++stage) {
        const double alpha = sinw / (2.0 * kButterworthQ[static_cast<std::size_t>(stage)]);
        const double a0 = 1.0 + alpha;
        if (std::fabs(a0) < 1e-12) {
            valid = false;
            break;
        }
        hpB0_[static_cast<std::size_t>(stage)] =
            static_cast<float>(((1.0 + cosw) * 0.5) / a0);
        hpB1_[static_cast<std::size_t>(stage)] =
            static_cast<float>((-(1.0 + cosw)) / a0);
        hpB2_[static_cast<std::size_t>(stage)] =
            hpB0_[static_cast<std::size_t>(stage)];
        hpA1_[static_cast<std::size_t>(stage)] =
            static_cast<float>((-2.0 * cosw) / a0);
        hpA2_[static_cast<std::size_t>(stage)] =
            static_cast<float>((1.0 - alpha) / a0);
    }
    if (!valid) {
        hpB0_.fill(0.0f);
        hpB1_.fill(0.0f);
        hpB2_.fill(0.0f);
        hpA1_.fill(0.0f);
        hpA2_.fill(0.0f);
        hpB0_[0] = 1.0f;
    }
    resetAnalysisHighPass();
}

void FeedbackSuppressorProcessor::closeGate() noexcept {
    // Drop every cut and every sample the detector has seen. When the gate
    // reopens the processor starts from silence instead of applying a stale
    // notch to the first transient.
    for (auto& notch : notches_) notch = Notch{};
    for (auto& freq : publishedFreqs_) freq.store(0.0f, std::memory_order_relaxed);
    for (auto& depth : publishedDepths_) depth.store(0.0f, std::memory_order_relaxed);
    publishedCount_.store(0, std::memory_order_relaxed);
    clearAnalysisHistory();
    gateOpen_ = false;
}

void FeedbackSuppressorProcessor::analyze() {
    if (sampleRate_ <= 0.0) return;
    detections_.clear();

    // High band: the fast 2048-point FFT still reacts every 10.7 ms. Its
    // lowest bin is ~23.4 Hz at 48 kHz, which is fine above the split.
    if (analysisCount_ >= kHighAnalysisSize) {
        const int n = kHighAnalysisSize;
        const int available = std::min(analysisCount_, n);
        const int first = n - available;
        for (int i = 0; i < n; ++i) {
            float sample = 0.0f;
            if (i >= first) {
                const int src = (analysisWrite_ - available + (i - first) + kLowAnalysisSize)
                              % kLowAnalysisSize;
                sample = analysisBuffer_[static_cast<std::size_t>(src)];
            }
            const float w = 0.5f - 0.5f * std::cos(
                2.0f * kPi * static_cast<float>(i) / static_cast<float>(n - 1));
            fftReal_[static_cast<std::size_t>(i)] = sample * w;
            fftImag_[static_cast<std::size_t>(i)] = 0.0f;
        }
        fft(fftReal_, fftImag_);
        for (int i = 1; i < n / 2; ++i) {
            mags_[static_cast<std::size_t>(i)] =
                std::sqrt(fftReal_[static_cast<std::size_t>(i)] * fftReal_[static_cast<std::size_t>(i)]
                        + fftImag_[static_cast<std::size_t>(i)] * fftImag_[static_cast<std::size_t>(i)]);
        }
        analyzeSpectrum(n, mags_,
                        std::max(effect_.detectorHighPassHz, kAnalysisSplitHz),
                        effect_.detectorLowPassHz);
    }

    // Low band: 8192 samples gives ~5.9 Hz bins at 48 kHz. That is enough
    // resolution to put a bass cut on the resonance instead of on a neighbour
    // 20+ Hz away. It needs 170 ms of history, so it starts later than the
    // high band and does not slow the high-band response.
    if (analysisCount_ >= kLowAnalysisSize) {
        const int n = kLowAnalysisSize;
        for (int i = 0; i < n; ++i) {
            const int src = (analysisWrite_ - n + i + kLowAnalysisSize) % kLowAnalysisSize;
            const float sample = analysisBuffer_[static_cast<std::size_t>(src)];
            const float w = 0.5f - 0.5f * std::cos(
                2.0f * kPi * static_cast<float>(i) / static_cast<float>(n - 1));
            fftLowReal_[static_cast<std::size_t>(i)] = sample * w;
            fftLowImag_[static_cast<std::size_t>(i)] = 0.0f;
        }
        fft(fftLowReal_, fftLowImag_);
        for (int i = 1; i < n / 2; ++i) {
            magsLow_[static_cast<std::size_t>(i)] =
                std::sqrt(fftLowReal_[static_cast<std::size_t>(i)] * fftLowReal_[static_cast<std::size_t>(i)]
                        + fftLowImag_[static_cast<std::size_t>(i)] * fftLowImag_[static_cast<std::size_t>(i)]);
        }
        analyzeSpectrum(n, magsLow_,
                        effect_.detectorHighPassHz,
                        std::min(effect_.detectorLowPassHz, kAnalysisSplitHz));
    }

    std::sort(detections_.begin(), detections_.end(),
              [](const Detection& a, const Detection& b) {
                  return a.levelDb > b.levelDb;
              });
    updateNotches(detections_);
}

void FeedbackSuppressorProcessor::analyzeSpectrum(
    int fftSize, const std::vector<float>& mags,
    float bandLowHz, float bandHighHz) {
    if (fftSize < 16 || mags.empty() || sampleRate_ <= 0.0) return;
    const int half = fftSize / 2;
    const float binWidth = static_cast<float>(sampleRate_) / static_cast<float>(fftSize);
    const int lowBin = std::max(1, static_cast<int>(std::ceil(bandLowHz / binWidth)));
    const int highBin = std::min(half - 1, static_cast<int>(std::floor(bandHighHz / binWidth)));
    if (highBin < lowBin) return;

    float sumMag = 0.0f;
    float bandMax = 0.0f;
    float globalMax = 0.0f;
    int bandMaxBin = lowBin;
    int globalMaxBin = 0;
    int magCount = 0;
    for (int i = 1; i < half; ++i) {
        const float mag = mags[static_cast<std::size_t>(i)];
        if (mag > globalMax) {
            globalMax = mag;
            globalMaxBin = i;
        }
        if (i >= lowBin && i <= highBin) {
            sumMag += mag;
            ++magCount;
            if (mag > bandMax) {
                bandMax = mag;
                bandMaxBin = i;
            }
        }
    }
    const float avg = magCount > 0 ? sumMag / static_cast<float>(magCount) : 0.0f;
    bandMax = std::max(bandMax, 1e-12f);
    const float referenceMax = fftSize == kLowAnalysisSize ? bandMax : globalMax;
    const float gateAmplitude = std::pow(10.0f, effect_.inputSensitivityDb / 20.0f);
    const float absoluteFloor = 0.25f * static_cast<float>(fftSize) * gateAmplitude * 0.5f;
    const float peakFloor = std::max({avg * 1.5f, bandMax * 0.02f, referenceMax * 0.05f, absoluteFloor});

    const int firstPeakBin = std::max(1, lowBin);
    const int lastPeakBin = std::min(half - 2, highBin);
    // The 8192-point low band resolves much narrower peaks, so a 4-bin
    // neighbourhood would include a neighbouring bass resonance and mask the
    // odd one out. Keep the high-band radius for noise rejection, but use a
    // tighter local baseline for the low band.
    const int prominenceRadius = fftSize == kLowAnalysisSize ? 2 : 4;
    constexpr int kGuardBins = 1;
    peaks_.clear();
    for (int i = firstPeakBin; i <= lastPeakBin; ++i) {
        const float center = mags[static_cast<std::size_t>(i)];
        if (center <= peakFloor) continue;
        if (center < mags[static_cast<std::size_t>(i - 1)]
            || center <= mags[static_cast<std::size_t>(i + 1)]) {
            continue;
        }

        std::array<float, 8> neighbours{};
        int neighbourCount = 0;
        for (int j = i - prominenceRadius; j <= i + prominenceRadius; ++j) {
            if (j < 1 || j >= half) continue;
            if (std::abs(j - i) <= kGuardBins) continue;
            neighbours[static_cast<std::size_t>(neighbourCount++)] =
                mags[static_cast<std::size_t>(j)];
        }
        float tonalityDb = 0.0f;
        const int minNeighbours = fftSize == kLowAnalysisSize ? 2 : 3;
        if (neighbourCount >= minNeighbours) {
            std::sort(neighbours.begin(), neighbours.begin() + neighbourCount);
            const float localMedian =
                neighbours[static_cast<std::size_t>(neighbourCount / 2)];
            tonalityDb = 20.0f * std::log10(
                std::max(1e-12f, center) / std::max(1e-12f, localMedian));
            // Always keep the strongest in-band peak: it is the one a relative
            // test must never discard, even when the spectrum around it is
            // dense or the peak is clipped. Noise is not a peak, though: if the
            // strongest bin is only a few dB above its neighbours, let the
            // ordinary tonality test reject it.
            if (i == bandMaxBin) {
                if (fftSize != kLowAnalysisSize && i != globalMaxBin && tonalityDb < 6.0f) continue;
                tonalityDb = std::max(tonalityDb, 6.0f);
            } else if (tonalityDb < 6.0f) {
                continue;
            }
        }

        const float regionThreshold = center * 0.5011872336f;
        int leftBin = i;
        while (leftBin > 1 && mags[static_cast<std::size_t>(leftBin - 1)] >= regionThreshold) {
            --leftBin;
        }
        int rightBin = i;
        while (rightBin < half - 1
               && mags[static_cast<std::size_t>(rightBin + 1)] >= regionThreshold) {
            ++rightBin;
        }

        const float left = std::log(std::max(1e-12f, mags[static_cast<std::size_t>(i - 1)]));
        const float middle = std::log(std::max(1e-12f, center));
        const float right = std::log(std::max(1e-12f, mags[static_cast<std::size_t>(i + 1)]));
        const float denom = left - 2.0f * middle + right;
        const bool plateau = mags[static_cast<std::size_t>(i - 1)] > center * 0.95f
                          || mags[static_cast<std::size_t>(i + 1)] > center * 0.95f;
        float offset = 0.0f;
        if (!plateau && denom < -1e-6f) {
            offset = 0.5f * (left - right) / denom;
            offset = std::clamp(offset, -0.5f, 0.5f);
            if (!std::isfinite(offset)) offset = 0.0f;
        }
        const float peakDbfs = 20.0f * std::log10(
            std::max(1e-12f, center) / (static_cast<float>(fftSize) * 0.25f));
        peaks_.push_back({center,
                          static_cast<float>(i) + offset,
                          static_cast<float>(leftBin),
                          static_cast<float>(rightBin),
                          tonalityDb,
                          peakDbfs});
    }
    std::sort(peaks_.begin(), peaks_.end(), [](const Peak& a, const Peak& b) {
        return a.magnitude > b.magnitude;
    });

    const int maxCuts = std::min(kMaxNotches, feedbackSuppressorCutCount(effect_.strength));
    const bool lowResolution = fftSize == kLowAnalysisSize;
    const float mergeHz = lowResolution
        ? binWidth * 1.25f
        : binWidth * 2.0f;
    for (const auto& peak : peaks_) {
        if (static_cast<int>(detections_.size()) >= maxCuts) break;
        const float f = peak.bin * static_cast<float>(sampleRate_) / static_cast<float>(fftSize);
        if (f < bandLowHz || f > bandHighHz) continue;

        bool duplicate = false;
        for (const auto& existing : detections_) {
            const bool sameScale =
                std::fabs(existing.binWidthHz - binWidth) < 0.01f;
            const bool regionsOverlap = sameScale
                                     && peak.leftBin <= existing.rightBin
                                     && peak.rightBin >= existing.leftBin;
            const bool closeByHz = std::fabs(existing.freq - f) < mergeHz;
            // At low frequencies a wider FFT makes it possible to resolve two
            // genuinely separate resonances whose -6 dB regions still touch.
            // Keep them only when there is a real valley between the peaks;
            // otherwise they are two samples of the same broad resonance.
            bool distinctValley = false;
            if (lowResolution && sameScale && regionsOverlap
                && std::fabs(existing.freq - f) >= binWidth * 0.9f) {
                const int halfSize = static_cast<int>(mags.size());
                int a = static_cast<int>(std::lround(existing.freq / binWidth));
                int b = static_cast<int>(std::lround(f / binWidth));
                a = std::clamp(a, 1, halfSize - 2);
                b = std::clamp(b, 1, halfSize - 2);
                if (a > b) std::swap(a, b);
                float valley = std::numeric_limits<float>::max();
                for (int k = a + 1; k < b; ++k) {
                    valley = std::min(valley, mags[static_cast<std::size_t>(k)]);
                }
                if (valley < std::numeric_limits<float>::max()) {
                    const float valleyDb = 20.0f * std::log10(
                        std::max(1e-12f, valley) / (static_cast<float>(fftSize) * 0.25f));
                    const float weakerDb = std::min(existing.levelDb, peak.levelDb);
                    distinctValley = valleyDb < weakerDb - 4.0f;
                }
            }
            if (closeByHz || (regionsOverlap && !distinctValley)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        detections_.push_back({f,
                               dynamicDepthDb(effect_.attenuationDb,
                                              peak.levelDb,
                                              effect_.inputSensitivityDb,
                                              effect_.gainReductionMultiplier),
                               peak.tonalityDb,
                               peak.levelDb,
                               peak.leftBin,
                               peak.rightBin,
                               binWidth});
    }
}

void FeedbackSuppressorProcessor::updateNotches(
    const std::vector<Detection>& detections) {
    ++analysisPass_;
    std::array<Notch, kMaxNotches> next{};
    std::array<bool, kMaxNotches> used{};
    int count = 0;
    const int maxCuts = std::min(kMaxNotches, feedbackSuppressorCutCount(effect_.strength));

    const float analysisBinWidth = sampleRate_ > 0.0
        ? static_cast<float>(sampleRate_ / static_cast<double>(kHighAnalysisSize))
        : 23.4f;

    // Match tolerances follow frequency instead of using one fixed Hz radius.
    // At 60 Hz a 2.5-bin radius is ~59 Hz and can merge two separate low
    // resonances; at 1 kHz the same radius is unnecessarily tight. One to two
    // bins keeps bin-to-bin movement of a single peak together while still
    // allowing close but distinct low tones to separate.
    const auto matchTolerance = [analysisBinWidth](float frequencyHz, float bandwidthHz) {
        const float bin = bandwidthHz > 0.0f ? bandwidthHz : analysisBinWidth;
        return std::clamp(frequencyHz * 0.03f, bin * 1.1f, bin * 1.5f);
    };

    // A new frequency must survive several detector passes before it earns a
    // cut. This is the main defence against short voice peaks and musical
    // transients. One or two missed hops are tolerated so a single noisy FFT
    // frame does not restart the whole confirmation window.
    const float intervalMs = 1000.0f * static_cast<float>(kAnalysisInterval)
        / static_cast<float>(std::max(1.0, sampleRate_));
    const int confirmPasses = std::max(
        1, static_cast<int>(std::ceil(effect_.detectionDelayMs / intervalMs)));
    const int allowedMisses = confirmPasses >= 4 ? 2 : (confirmPasses >= 2 ? 1 : 0);

    // Louder peaks are already well clear of the input gate, so they need less
    // confirmation than a quiet peak hovering just above it. Scaling by the
    // gate-to-peak amplitude ratio makes strong feedback engage immediately
    // while still protecting against quiet voice/music peaks.
    const float gateAmplitude = std::pow(10.0f, effect_.inputSensitivityDb / 20.0f);
    const auto confirmPassesForLevel = [confirmPasses, gateAmplitude](float levelDb) {
        const float peakAmplitude = std::pow(10.0f, levelDb / 20.0f);
        const float scale = std::clamp(gateAmplitude / std::max(1.0e-9f, peakAmplitude),
                                       0.35f, 1.5f);
        return std::max(1, static_cast<int>(std::ceil(
            static_cast<float>(confirmPasses) * scale)));
    };
    const int maxCandidateHits = std::max(confirmPasses * 2 + 2, confirmPasses + 4);

    // Keep existing cuts first. A detection that lands on an active cut only
    // refreshes its Hold and depth; its frequency glides toward the detection
    // instead of hard-locking to the first estimate.
    const int detectionCount = std::min(static_cast<int>(detections.size()), kMaxNotches);
    for (int p = 0; p < detectionCount && count < maxCuts; ++p) {
        const auto& detection = detections[static_cast<std::size_t>(p)];
        const float tolerance = matchTolerance(detection.freq, detection.binWidthHz);
        bool matched = false;
        for (int i = 0; i < kMaxNotches; ++i) {
            if (used[static_cast<std::size_t>(i)]) continue;
            const auto& old = notches_[static_cast<std::size_t>(i)];
            if (!old.active || std::fabs(old.freq - detection.freq) > tolerance) continue;

            Notch notch = old;
            notch.targetDepthDb = detection.depthDb;
            notch.depthDb += 0.25f * (detection.depthDb - notch.depthDb);
            if (detection.binWidthHz > 0.0f) notch.binWidthHz = detection.binWidthHz;
            // Follow a moving resonance in small steps. The cap is half a bin
            // per analysis pass, which converges quickly without jumping to a
            // neighboring harmonic or chasing noise.
            const float maxStepHz = notch.binWidthHz * 0.5f;
            const float correction = std::clamp(0.45f * (detection.freq - notch.freq),
                                                -maxStepHz, maxStepHz);
            notch.freq = std::clamp(notch.freq + correction,
                                    effect_.detectorHighPassHz,
                                    effect_.detectorLowPassHz);
            notch.target = 1.0f;
            notch.holdSamples = holdSamples_;
            used[static_cast<std::size_t>(i)] = true;
            updateNotchCoefficients(notch);
            next[static_cast<std::size_t>(count++)] = notch;
            matched = true;
            break;
        }
        if (matched) continue;

        // Not an existing cut: track it as a candidate. Candidate frequency
        // and tonality are smoothed over hits, which keeps a bin-boundary peak
        // from wobbling on every FFT pass.
        int candidateIndex = -1;
        for (int i = 0; i < kMaxNotches; ++i) {
            auto& candidate = candidates_[static_cast<std::size_t>(i)];
            if (candidate.hits <= 0) continue;
            if (candidate.lastSeenPass == analysisPass_) continue;
            const float candidateTolerance =
                matchTolerance(candidate.freq, candidate.binWidthHz);
            if (std::fabs(candidate.freq - detection.freq) <= candidateTolerance) {
                candidateIndex = i;
                break;
            }
        }
        if (candidateIndex < 0) {
            for (int i = 0; i < kMaxNotches; ++i) {
                if (candidates_[static_cast<std::size_t>(i)].hits <= 0) {
                    candidateIndex = i;
                    break;
                }
            }
        }
        if (candidateIndex >= 0) {
            auto& candidate = candidates_[static_cast<std::size_t>(candidateIndex)];
            if (candidate.hits <= 0) {
                candidate = Candidate{};
                candidate.freq = detection.freq;
                candidate.firstLevelDb = detection.levelDb;
                candidate.binWidthHz = detection.binWidthHz;
            } else {
                candidate.freq = 0.7f * candidate.freq + 0.3f * detection.freq;
                if (detection.binWidthHz > 0.0f) candidate.binWidthHz = detection.binWidthHz;
            }
            candidate.depthDb = detection.depthDb;
            candidate.tonalityDb = candidate.hits <= 0
                ? detection.tonalityDb
                : 0.7f * candidate.tonalityDb + 0.3f * detection.tonalityDb;
            candidate.lastLevelDb = detection.levelDb;
            candidate.hits = std::min(maxCandidateHits, candidate.hits + 1);
            candidate.misses = 0;
            candidate.lastSeenPass = analysisPass_;
        }
    }

    // Preserve unmatched cuts through Hold, then Release. They keep their
    // established frequency; a nearby new detection does not make them jump.
    for (int i = 0; i < kMaxNotches && count < maxCuts; ++i) {
        if (used[static_cast<std::size_t>(i)]) continue;
        const auto& old = notches_[static_cast<std::size_t>(i)];
        if (!old.active) continue;
        Notch notch = old;
        if (notch.holdSamples > 0) {
            notch.holdSamples = std::max(0, notch.holdSamples - kAnalysisInterval);
            notch.target = 1.0f;
        } else {
            notch.target = 0.0f;
        }
        if (notch.target <= 0.0f && notch.envelope <= 0.0001f) continue;
        updateNotchCoefficients(notch);
        next[static_cast<std::size_t>(count++)] = notch;
    }

    // Age candidates that were not seen in this pass. A couple of misses are
    // allowed; more than that means the peak is gone, not merely noisy.
    for (int i = 0; i < kMaxNotches; ++i) {
        auto& candidate = candidates_[static_cast<std::size_t>(i)];
        if (candidate.hits <= 0 || candidate.lastSeenPass == analysisPass_) continue;
        ++candidate.misses;
        if (candidate.misses > allowedMisses) candidate = Candidate{};
    }

    const auto candidateIsEligible = [&](const Candidate& candidate) {
        if (candidate.hits <= 0) return false;
        const float growthDb = candidate.lastLevelDb - candidate.firstLevelDb;
        const int candidateConfirmPasses = confirmPassesForLevel(candidate.lastLevelDb);
        const int requiredHits = std::max(
            1, candidateConfirmPasses - (growthDb >= 3.0f ? 1 : 0));
        const int totalPasses = candidate.hits + candidate.misses;
        const float hitRatio = totalPasses > 0
            ? static_cast<float>(candidate.hits) / static_cast<float>(totalPasses)
            : 0.0f;
        return totalPasses >= candidateConfirmPasses
            && candidate.hits >= requiredHits
            && hitRatio >= 0.8f
            && candidate.tonalityDb >= 5.0f;
    };

    const auto makeNotchFromCandidate = [](const Candidate& candidate) {
        Notch notch;
        notch.active = true;
        notch.freq = candidate.freq;
        notch.depthDb = candidate.depthDb;
        notch.targetDepthDb = candidate.depthDb;
        notch.target = 1.0f;
        notch.binWidthHz = candidate.binWidthHz > 0.0f
            ? candidate.binWidthHz
            : 23.4f;
        return notch;
    };

    // Promote confirmed new candidates after active and held cuts have their
    // slots. If the rack is full, a clearly stronger candidate may replace a
    // held cut that is already halfway through its hold; otherwise Hold stays
    // authoritative.
    for (int i = 0; i < kMaxNotches; ++i) {
        auto& candidate = candidates_[static_cast<std::size_t>(i)];
        if (!candidateIsEligible(candidate)) continue;

        // A confirmed candidate may be a bin-boundary sibling of a cut that
        // was just promoted in this pass. Merge it into that cut instead of
        // opening a second notch beside the same resonance.
        bool mergedIntoExisting = false;
        for (int j = 0; j < count; ++j) {
            auto& existing = next[static_cast<std::size_t>(j)];
            if (existing.target < 1.0f) continue;
            if (std::fabs(existing.freq - candidate.freq)
                    > matchTolerance(candidate.freq, candidate.binWidthHz)) continue;
            existing.targetDepthDb = candidate.depthDb;
            existing.depthDb = 0.25f * existing.depthDb + 0.75f * candidate.depthDb;
            existing.target = 1.0f;
            existing.holdSamples = holdSamples_;
            updateNotchCoefficients(existing);
            candidate = Candidate{};
            mergedIntoExisting = true;
            break;
        }
        if (mergedIntoExisting) continue;

        if (count < maxCuts) {
            Notch notch = makeNotchFromCandidate(candidate);
            notch.holdSamples = holdSamples_;
            updateNotchCoefficients(notch);
            next[static_cast<std::size_t>(count++)] = notch;
            candidate = Candidate{};
            continue;
        }

        if (effect_.holdMs <= 0.0f) continue;
        int replaceIndex = -1;
        float weakestDepth = 1.0e9f;
        for (int j = 0; j < count; ++j) {
            const auto& held = next[static_cast<std::size_t>(j)];
            if (held.holdSamples <= 0 || held.holdSamples > holdSamples_ / 2) continue;
            if (held.depthDb < weakestDepth) {
                weakestDepth = held.depthDb;
                replaceIndex = j;
            }
        }
        if (replaceIndex < 0 || candidate.depthDb <= weakestDepth + 6.0f) continue;

        Notch notch = makeNotchFromCandidate(candidate);
        notch.holdSamples = holdSamples_;
        updateNotchCoefficients(notch);
        next[static_cast<std::size_t>(replaceIndex)] = notch;
        candidate = Candidate{};
    }

    // Final separation pass: a bin-boundary sibling can still reach this point
    // as a separate candidate. Keep one cut per resonance region so the UI
    // never shows two notches on either side of the same peak.
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ) {
            const float bin = next[static_cast<std::size_t>(i)].binWidthHz > 0.0f
                ? next[static_cast<std::size_t>(i)].binWidthHz
                : analysisBinWidth;
            const float separation = bin * (bin < 12.0f ? 1.25f : 2.0f);
            if (std::fabs(next[static_cast<std::size_t>(i)].freq
                          - next[static_cast<std::size_t>(j)].freq)
                    <= separation) {
                if (next[static_cast<std::size_t>(j)].depthDb
                        > next[static_cast<std::size_t>(i)].depthDb) {
                    next[static_cast<std::size_t>(i)] = next[static_cast<std::size_t>(j)];
                }
                next[static_cast<std::size_t>(j)] = next[static_cast<std::size_t>(count - 1)];
                --count;
                continue;
            }
            ++j;
        }
    }

    notches_ = next;
    for (int i = 0; i < kMaxNotches; ++i) {
        publishedFreqs_[static_cast<std::size_t>(i)].store(
            i < count ? next[static_cast<std::size_t>(i)].freq : 0.0f,
            std::memory_order_relaxed);
        publishedDepths_[static_cast<std::size_t>(i)].store(
            i < count ? next[static_cast<std::size_t>(i)].depthDb
                            * next[static_cast<std::size_t>(i)].envelope
                      : 0.0f,
            std::memory_order_relaxed);
    }
    publishedCount_.store(count, std::memory_order_relaxed);
}

void FeedbackSuppressorProcessor::process(float* left, float* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0) return;
    if (effect_.bypassed) {
        publishedCount_.store(0, std::memory_order_relaxed);
        reductionDb_ = 0.0f;
        resetAnalysisHighPass();
        resetAnalysisLowPass();
        return;
    }

    // Input sensitivity is a hard gate on the block RMS. Below it the detector
    // stops learning so room noise and converter hiss cannot create a notch.
    // Existing cuts are still allowed to finish Hold and Release smoothly
    // instead of disappearing the instant the level dips.
    double energy = 0.0;
    for (int i = 0; i < frames; ++i) {
        energy += static_cast<double>(left[i]) * left[i]
                + static_cast<double>(right[i]) * right[i];
    }
    const float blockRms = static_cast<float>(
        std::sqrt(energy / (2.0 * static_cast<double>(frames))));
    const bool detectorOpen = blockRms >= sensitivityLinear_;

    bool hasActiveNotches = false;
    for (const auto& notch : notches_) {
        if (notch.active) { hasActiveNotches = true; break; }
    }
    if (!detectorOpen && !hasActiveNotches) {
        if (gateOpen_) closeGate();
        return;
    }
    if (!detectorOpen && gateOpen_) {
        // The level just fell below the gate. Keep the cuts alive so their
        // hold/release can run, but discard the detector history so a later
        // reopen does not analyze stale samples.
        clearAnalysisHistory();
    }
    gateOpen_ = detectorOpen;

    const int n = kLowAnalysisSize;
    // Analyze inside large host blocks too. A single 16k-frame call would
    // otherwise finish before the detector ever ran, leaving the first block
    // untouched and delaying every cut until the next callback.
    for (int start = 0; start < frames; start += kAnalysisInterval) {
        const int chunk = std::min(kAnalysisInterval, frames - start);
        for (int i = 0; i < chunk; ++i) {
            const int index = start + i;
            const float inL = left[index];
            const float inR = right[index];
            float xL = inL;
            float xR = inR;
            for (auto& notch : notches_) {
                if (!notch.active) continue;
                // The peaking biquad is designed for the requested depth. The
                // envelope blends dry into that filtered signal over Attack,
                // then back to dry after Hold expires.
                const float gain = notch.envelope;
                const float dryL = xL;
                const float dryR = xR;
                const float yL = notch.b0 * dryL + notch.z1L;
                notch.z1L = notch.b1 * dryL - notch.a1 * yL + notch.z2L;
                notch.z2L = notch.b2 * dryL - notch.a2 * yL;
                xL = dryL + gain * (yL - dryL);

                const float yR = notch.b0 * dryR + notch.z1R;
                notch.z1R = notch.b1 * dryR - notch.a1 * yR + notch.z2R;
                notch.z2R = notch.b2 * dryR - notch.a2 * yR;
                xR = dryR + gain * (yR - dryR);

                if (notch.target > notch.envelope) {
                    notch.envelope = std::min(notch.target, notch.envelope + attackStep_);
                } else if (notch.target < notch.envelope) {
                    notch.envelope = std::max(notch.target, notch.envelope - releaseStep_);
                }
            }
            left[index] = xL;
            right[index] = xR;

            // Analysis buffer update (mono mix). While the gate is closed we
            // do not feed the detector; the cuts are only releasing.
            if (detectorOpen) {
                const float mono = 0.5f * (inL + inR);
                float hp = mono;
                for (int stage = 0; stage < 4; ++stage) {
                    const auto s = static_cast<std::size_t>(stage);
                    const float y = hpB0_[s] * hp + hpZ1_[s];
                    hpZ1_[s] = hpB1_[s] * hp - hpA1_[s] * y + hpZ2_[s];
                    hpZ2_[s] = hpB2_[s] * hp - hpA2_[s] * y;
                    hp = y;
                }
                float lp = hp;
                for (int stage = 0; stage < 4; ++stage) {
                    const auto s = static_cast<std::size_t>(stage);
                    const float y = lpB0_[s] * lp + lpZ1_[s];
                    lpZ1_[s] = lpB1_[s] * lp - lpA1_[s] * y + lpZ2_[s];
                    lpZ2_[s] = lpB2_[s] * lp - lpA2_[s] * y;
                    lp = y;
                }
                analysisBuffer_[analysisWrite_] = lp;
                analysisWrite_ = (analysisWrite_ + 1) % n;
                if (analysisCount_ < n) ++analysisCount_;
            }

            // RMS tracking.
            inputRmsAccum_ += inL * inL + inR * inR;
            outputRmsAccum_ += xL * xL + xR * xR;
            ++rmsCount_;
        }

        framesSinceAnalysis_ += chunk;
        if (detectorOpen) {
            if (analysisCount_ >= kHighAnalysisSize && framesSinceAnalysis_ >= kAnalysisInterval) {
                analyze();
                framesSinceAnalysis_ = 0;
            }
        } else if (framesSinceAnalysis_ >= kAnalysisInterval) {
            // No new detections: advance hold/release on the surviving cuts.
            detections_.clear();
            updateNotches(detections_);
            framesSinceAnalysis_ = 0;
        }
    }

    if (rmsCount_ > 0) {
        const float inRms = std::sqrt(inputRmsAccum_ / static_cast<float>(rmsCount_ * 2));
        const float outRms = std::sqrt(outputRmsAccum_ / static_cast<float>(rmsCount_ * 2));
        if (outRms > 1e-12f && inRms > outRms) {
            reductionDb_ = 20.0f * std::log10(inRms / outRms);
        } else {
            reductionDb_ = 0.0f;
        }
        // Smooth a bit.
        inputRmsAccum_ = 0.0f;
        outputRmsAccum_ = 0.0f;
        rmsCount_ = 0;
    }
}

} // namespace pluginrack
