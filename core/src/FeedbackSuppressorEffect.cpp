#include "pluginrack/FeedbackSuppressorEffect.h"

#include <algorithm>
#include <cmath>
#include <complex>
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
} // namespace

FeedbackSuppressorEffect makeDefaultFeedbackSuppressorEffect(std::uint32_t id) {
    FeedbackSuppressorEffect effect;
    effect.id = id;
    effect.bypassed = false;
    effect.strength = 0.5f;
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
    analysisBuffer_.assign(kAnalysisSize, 0.0f);
    fftReal_.assign(kAnalysisSize, 0.0f);
    fftImag_.assign(kAnalysisSize, 0.0f);
    analysisWrite_ = 0;
    analysisCount_ = 0;
    framesSinceAnalysis_ = 0;
    reductionDb_ = 0.0f;
    inputRmsAccum_ = 0.0f;
    outputRmsAccum_ = 0.0f;
    rmsCount_ = 0;
    for (auto& n : notches_) n = Notch{};
    setEffect(effect_);
}

void FeedbackSuppressorProcessor::setEffect(const FeedbackSuppressorEffect& effect) {
    effect_ = effect;
    if (effect_.strength < 0.0f) effect_.strength = 0.0f;
    if (effect_.strength > 1.0f) effect_.strength = 1.0f;
}

void FeedbackSuppressorProcessor::reset() noexcept {
    for (auto& n : notches_) {
        n.z1L = n.z2L = n.z1R = n.z2R = 0.0f;
    }
    analysisWrite_ = 0;
    analysisCount_ = 0;
    framesSinceAnalysis_ = 0;
    reductionDb_ = 0.0f;
    inputRmsAccum_ = 0.0f;
    outputRmsAccum_ = 0.0f;
    rmsCount_ = 0;
    std::fill(analysisBuffer_.begin(), analysisBuffer_.end(), 0.0f);
}

void FeedbackSuppressorProcessor::analyze() {
    // Build windowed mono buffer in FFT arrays.
    const int n = kAnalysisSize;
    for (int i = 0; i < n; ++i) {
        // analysisWrite_ points to next write; oldest is analysisWrite_.
        // We stored circularly; reconstruct oldest-first.
        const int src = (analysisWrite_ + i) % n;
        const float sample = analysisBuffer_[src];
        const float w = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(n - 1));
        fftReal_[i] = sample * w;
        fftImag_[i] = 0.0f;
    }

    fft(fftReal_, fftImag_);

    // Magnitudes for bins 1..n/2-1.
    std::vector<std::pair<float, int>> peaks;
    peaks.reserve(n / 2);
    const int half = n / 2;
    float sumMag = 0.0f;
    std::vector<float> mags(half);
    for (int i = 1; i < half; ++i) {
        const float mag = std::sqrt(fftReal_[i] * fftReal_[i] + fftImag_[i] * fftImag_[i]);
        mags[i] = mag;
        sumMag += mag;
    }
    const float avg = sumMag / static_cast<float>(half - 1);

    // Find local maxima above 1.5x average.
    for (int i = 2; i < half - 1; ++i) {
        if (mags[i] > mags[i - 1] && mags[i] > mags[i + 1] && mags[i] > avg * 1.5f) {
            peaks.push_back({mags[i], i});
        }
    }
    std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });

    const int maxNotches = effect_.strength < 0.05f ? 0
        : std::min(kMaxNotches, std::max(1, static_cast<int>(std::round(effect_.strength * 8.0f))));

    std::vector<float> freqs;
    freqs.reserve(maxNotches);
    for (int i = 0; i < static_cast<int>(peaks.size()) && static_cast<int>(freqs.size()) < maxNotches; ++i) {
        const float f = peaks[i].second * static_cast<float>(sampleRate_) / static_cast<float>(n);
        if (f < 80.0f || f > 8000.0f) continue;
        // Avoid duplicates within 20 Hz.
        bool dup = false;
        for (float existing : freqs) {
            if (std::fabs(existing - f) < 20.0f) { dup = true; break; }
        }
        if (dup) continue;
        freqs.push_back(f);
    }

    updateNotches(freqs);
}

void FeedbackSuppressorProcessor::updateNotches(const std::vector<float>& peakFreqs) {
    // Deactivate all first.
    for (auto& n : notches_) {
        n.active = false;
        n.z1L = n.z2L = n.z1R = n.z2R = 0.0f;
    }
    const float Q = 24.0f + effect_.strength * 16.0f;
    for (int i = 0; i < static_cast<int>(peakFreqs.size()) && i < kMaxNotches; ++i) {
        auto& notch = notches_[i];
        notch.active = true;
        notch.freq = peakFreqs[i];
        const float w0 = 2.0f * kPi * notch.freq / static_cast<float>(sampleRate_);
        const float alpha = std::sin(w0) / (2.0f * Q);
        const float cosw = std::cos(w0);
        const float a0 = 1.0f + alpha;
        notch.b0 = 1.0f / a0;
        notch.b1 = -2.0f * cosw / a0;
        notch.b2 = 1.0f / a0;
        notch.a1 = -2.0f * cosw / a0;
        notch.a2 = (1.0f - alpha) / a0;
    }
}

void FeedbackSuppressorProcessor::process(float* left, float* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0) return;
    if (effect_.bypassed) {
        // Still update analysis? Keep passive.
        return;
    }

    const int n = kAnalysisSize;
    for (int i = 0; i < frames; ++i) {
        const float inL = left[i];
        const float inR = right[i];
        float xL = inL;
        float xR = inR;
        for (auto& notch : notches_) {
            if (!notch.active) continue;
            const float yL = notch.b0 * xL + notch.z1L;
            notch.z1L = notch.b1 * xL - notch.a1 * yL + notch.z2L;
            notch.z2L = notch.b2 * xL - notch.a2 * yL;
            xL = yL;

            const float yR = notch.b0 * xR + notch.z1R;
            notch.z1R = notch.b1 * xR - notch.a1 * yR + notch.z2R;
            notch.z2R = notch.b2 * xR - notch.a2 * yR;
            xR = yR;
        }
        left[i] = xL;
        right[i] = xR;

        // Analysis buffer update (mono mix).
        const float mono = 0.5f * (inL + inR);
        analysisBuffer_[analysisWrite_] = mono;
        analysisWrite_ = (analysisWrite_ + 1) % n;
        if (analysisCount_ < n) ++analysisCount_;

        // RMS tracking.
        inputRmsAccum_ += inL * inL + inR * inR;
        outputRmsAccum_ += xL * xL + xR * xR;
        ++rmsCount_;
    }

    framesSinceAnalysis_ += frames;
    if (analysisCount_ >= n && framesSinceAnalysis_ >= kAnalysisInterval) {
        analyze();
        framesSinceAnalysis_ = 0;
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