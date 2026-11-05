#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pluginrack {

// Clock-drift correction for input captures and non-clock outputs.
//
// The previous reader steered its rate by up to +-0.5 % and interpolated
// linearly between two neighbours. Linear interpolation smears the top end
// even at that depth, and a proportional-only correction leaves a standing
// offset in the FIFO. This version keeps the same rate limit but reads with a
// four-tap Catmull-Rom kernel and steers with a leaky PI loop, so the queue can
// be held shallower: the integrator removes the residual fill error, and the
// leak lets a stale correction drain once the error is gone instead of parking
// the ratio off-centre.
class DriftResampler {
public:
    struct Config {
        double maxCorrection = 0.005;  // +- ratio limit, 0.5 % default
        double kp = 0.02;              // proportional gain per second
        double ki = 0.002;             // integral gain per second
        double maxIntegral = 0.004;    // clamp on the integral term
    };

    // Re-clamps the live state into the new limits so ratio() can never report
    // a value the new configuration forbids. The gains are deliberately not
    // reset: retuning mid-stream must not step the read position.
    void configure(const Config& config) noexcept {
        const Config defaults{};
        maxCorrection_ = std::isfinite(config.maxCorrection)
            ? std::clamp(config.maxCorrection, 0.0, 0.05)
            : defaults.maxCorrection;
        kp_ = std::isfinite(config.kp) ? std::max(0.0, config.kp) : defaults.kp;
        ki_ = std::isfinite(config.ki) ? std::max(0.0, config.ki) : defaults.ki;
        maxIntegral_ = std::isfinite(config.maxIntegral)
            ? std::clamp(config.maxIntegral, 0.0, 1.0)
            : defaults.maxIntegral;
        integral_ = std::clamp(integral_, -maxIntegral_, maxIntegral_);
        ratio_ = 1.0 + std::clamp(ratio_ - 1.0, -maxCorrection_, maxCorrection_);
    }

    void reset() noexcept {
        integral_ = 0.0;
        ratio_ = 1.0;
    }

    // fillErrorSamples: actual fill minus target fill.
    // targetFillSamples: the target fill, used only to normalise the error.
    // dtSeconds: time since the previous call, clamped into [0.001, 1.0].
    // Returns the source-samples-per-output-sample ratio, clamped to
    // 1 +- maxCorrection. Non-finite input returns the previous ratio so a
    // single bad measurement cannot throw the read position off.
    double updateRatio(double fillErrorSamples, double targetFillSamples,
                       double dtSeconds) noexcept {
        if (!std::isfinite(fillErrorSamples) || !std::isfinite(targetFillSamples)
            || !std::isfinite(dtSeconds)) {
            return ratio_;
        }

        const double dt = std::clamp(dtSeconds, kMinDt, kMaxDt);
        // Normalising by at least one sample keeps an empty or tiny target from
        // turning a one-sample error into full scale (or dividing by zero).
        const double target = std::max(1.0, targetFillSamples);
        const double error = fillErrorSamples / target;

        // Leak before integrating: a saturated correction must decay on its own
        // once the fill error returns to zero, otherwise the ratio stays parked
        // at its limit and the queue can never re-centre.
        integral_ *= 1.0 - kLeakPerSecond * dt;
        integral_ += ki_ * error * dt;
        integral_ = std::clamp(integral_, -maxIntegral_, maxIntegral_);

        const double correction = kp_ * error + integral_;
        ratio_ = 1.0 + std::clamp(correction, -maxCorrection_, maxCorrection_);
        return ratio_;
    }

    double ratio() const noexcept { return ratio_; }
    double integral() const noexcept { return integral_; }

    // Cubic Hermite interpolation over a wrapping ring.
    // ring: channel data, `capacity` samples long. startIndex in [0, capacity).
    // Produces `frames` output samples in dest using `ratio`, advancing by
    // ratio per output sample. Requires capacity >= 4 and dest sized for
    // `frames`. noexcept, allocation-free.
    static void interpolateCubic(const float* ring, int capacity, int startIndex,
                                 int frames, double ratio, float* dest) noexcept {
        // Fewer than four taps cannot form the kernel, so the caller gets
        // silence instead of a read past the end of the ring.
        if (ring == nullptr || dest == nullptr || capacity < 4 || frames <= 0
            || !std::isfinite(ratio)) {
            writeZeros(dest, frames);
            return;
        }

        // Real corrections live inside +-5 %; clamping a nonsensical ratio keeps
        // the whole-sample advance below well inside int64 range.
        const double step = std::clamp(ratio, -kMaxStep, kMaxStep);

        std::int64_t whole = static_cast<std::int64_t>(startIndex);
        double phase = 0.0;
        for (int frame = 0; frame < frames; ++frame) {
            if (frame > 0) {
                phase += step;
                const double advanced = std::floor(phase);
                phase -= advanced;
                whole += static_cast<std::int64_t>(advanced);
            }

            const double p0 = static_cast<double>(ring[wrapIndex(whole - 1, capacity)]);
            const double p1 = static_cast<double>(ring[wrapIndex(whole, capacity)]);
            const double p2 = static_cast<double>(ring[wrapIndex(whole + 1, capacity)]);
            const double p3 = static_cast<double>(ring[wrapIndex(whole + 2, capacity)]);

            // Catmull-Rom form of the Hermite basis. At phase 0 it collapses to
            // p1, so a unity ratio reads the ring back sample for sample and the
            // no-drift fast path stays bit-faithful.
            const double a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
            const double b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
            const double c = -0.5 * p0 + 0.5 * p2;
            const double value = ((a * phase + b) * phase + c) * phase + p1;

            // A NaN or Inf tap would otherwise ring through the whole kernel and
            // then through the caller's mix bus, so one bad sample stays local.
            dest[frame] = std::isfinite(value) ? static_cast<float>(value) : 0.0f;
        }
    }

private:
    // 1/s: fast enough that a stale offset cannot hold the ratio off-centre
    // for more than a few seconds, slow compared with any block rate.
    static constexpr double kLeakPerSecond = 1.0;
    static constexpr double kMinDt = 0.001;
    static constexpr double kMaxDt = 1.0;
    static constexpr double kMaxStep = 1024.0;

    static int wrapIndex(std::int64_t index, int capacity) noexcept {
        const auto span = static_cast<std::int64_t>(capacity);
        const std::int64_t wrapped = index % span;
        return static_cast<int>(wrapped < 0 ? wrapped + span : wrapped);
    }

    static void writeZeros(float* dest, int frames) noexcept {
        if (dest == nullptr || frames <= 0) return;
        for (int frame = 0; frame < frames; ++frame) dest[frame] = 0.0f;
    }

    double maxCorrection_ = 0.005;
    double kp_ = 0.02;
    double ki_ = 0.002;
    double maxIntegral_ = 0.004;
    double integral_ = 0.0;
    double ratio_ = 1.0;
};

} // namespace pluginrack
