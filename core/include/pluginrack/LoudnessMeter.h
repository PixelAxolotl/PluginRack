#pragma once

#include "DspMath.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {

// Momentary loudness (LUFS-S) of one stereo strip. The signal runs through the
// BS.1770 K-weighting, is averaged over 400 ms and offset by the standard
// -0.691 dB. The filters are rebuilt for the device sample rate from the
// standard's analog prototypes (shelf 1682 Hz, +4 dB; high-pass 38 Hz), so a
// 96 kHz device reads the same loudness as a 48 kHz one.
//
// Not thread-safe: one instance per strip, updated by the render thread and
// published to the UI through an atomic by the caller.
class LoudnessMeter {
public:
    static constexpr double kWindowSeconds = 0.4;
    static constexpr float kSilenceLufs = -70.0f;
    static constexpr float kMaxLufs = 6.0f;

    void reset() noexcept {
        for (auto& channel : state_) {
            for (auto& stage : channel) {
                stage[0] = 0.0f;
                stage[1] = 0.0f;
            }
        }
        power_ = 0.0;
        prepared_ = false;
    }

    // Processes one block and returns the updated momentary loudness. Both
    // pointers may be null, which counts as silence for that side.
    float process(const float* left, const float* right, int frames,
                  double sampleRate) noexcept {
        if (frames <= 0 || sampleRate <= 0.0) {
            return currentLufs();
        }
        prepare(sampleRate);

        double weighted = 0.0;
        weighted += accumulate(0, left, frames);
        weighted += accumulate(1, right, frames);
        weighted /= static_cast<double>(frames) * 2.0;
        if (!std::isfinite(weighted)) {
            reset();
            return currentLufs();
        }

        const double blend = 1.0 - std::exp(-static_cast<double>(frames)
                                            / (kWindowSeconds * sampleRate));
        power_ += (weighted - power_) * blend;
        return currentLufs();
    }

    float currentLufs() const noexcept {
        if (!(power_ > 1.0e-10)) {
            return kSilenceLufs;
        }
        const double lufs = -0.691 + 10.0 * std::log10(power_);
        return static_cast<float>(std::clamp(lufs, static_cast<double>(kSilenceLufs),
                                             static_cast<double>(kMaxLufs)));
    }

private:
    void prepare(double sampleRate) noexcept {
        if (prepared_ && rate_ == sampleRate) {
            return;
        }
        // Analog prototypes from BS.1770-4, bilinear-transformed at the rate in
        // use. At 48 kHz this reproduces the published tables; the design also
        // keeps the weighting correct at 44.1/96/192 kHz instead of detuning
        // the fixed 48 kHz coefficients.
        constexpr double kShelfF0 = 1681.974450955533;
        constexpr double kShelfGainDb = 3.999843853973347;
        constexpr double kShelfQ = 0.7071752369554196;
        constexpr double kHighPassF0 = 38.13547087602444;
        constexpr double kHighPassQ = 0.5003270373238773;
        constexpr double kPi = 3.14159265358979323846;

        const double kShelf = std::tan(kPi * kShelfF0 / sampleRate);
        const double vh = std::pow(10.0, kShelfGainDb / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double shelfA0 = 1.0 + kShelf / kShelfQ + kShelf * kShelf;
        shelf_ = dsp::Biquad{
            static_cast<float>((vh + vb * kShelf / kShelfQ + kShelf * kShelf) / shelfA0),
            static_cast<float>(2.0 * (kShelf * kShelf - vh) / shelfA0),
            static_cast<float>((vh - vb * kShelf / kShelfQ + kShelf * kShelf) / shelfA0),
            static_cast<float>(2.0 * (kShelf * kShelf - 1.0) / shelfA0),
            static_cast<float>((1.0 - kShelf / kShelfQ + kShelf * kShelf) / shelfA0)};

        const double kHighPass = std::tan(kPi * kHighPassF0 / sampleRate);
        const double highPassA0 = 1.0 + kHighPass / kHighPassQ + kHighPass * kHighPass;
        hpf_ = dsp::Biquad{
            static_cast<float>(1.0 / highPassA0),
            static_cast<float>(-2.0 / highPassA0),
            static_cast<float>(1.0 / highPassA0),
            static_cast<float>(2.0 * (kHighPass * kHighPass - 1.0) / highPassA0),
            static_cast<float>((1.0 - kHighPass / kHighPassQ + kHighPass * kHighPass)
                               / highPassA0)};

        // A rate change invalidates the filter state of the old design.
        for (auto& channel : state_)
            for (auto& stage : channel) {
                stage[0] = 0.0f;
                stage[1] = 0.0f;
            }
        rate_ = sampleRate;
        prepared_ = true;
    }

    double accumulate(int channel, const float* data, int frames) noexcept {
        if (data == nullptr) {
            return 0.0;
        }
        float& shelfZ1 = state_[channel][0][0];
        float& shelfZ2 = state_[channel][0][1];
        float& hpfZ1 = state_[channel][1][0];
        float& hpfZ2 = state_[channel][1][1];
        double sum = 0.0;
        for (int i = 0; i < frames; ++i) {
            const float x = data[i];
            const float shelf = shelf_.b0 * x + shelfZ1;
            shelfZ1 = shelf_.b1 * x - shelf_.a1 * shelf + shelfZ2;
            shelfZ2 = shelf_.b2 * x - shelf_.a2 * shelf;
            const float weight = hpf_.b0 * shelf + hpfZ1;
            hpfZ1 = hpf_.b1 * shelf - hpf_.a1 * weight + hpfZ2;
            hpfZ2 = hpf_.b2 * shelf - hpf_.a2 * weight;
            sum += static_cast<double>(weight) * weight;
        }
        return sum;
    }

    dsp::Biquad shelf_{};
    dsp::Biquad hpf_{};
    // [channel][stage][z1 or z2]
    float state_[2][2][2]{};
    double rate_ = 0.0;
    double power_ = 0.0;
    bool prepared_ = false;
};

} // namespace pluginrack
