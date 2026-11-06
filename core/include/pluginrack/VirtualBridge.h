#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "pluginrack/DriftResampler.h"
#include "pluginrack/VirtualAudioProtocol.h"
#include "pluginrack/VirtualRing.h"

namespace pluginrack::virt {

// User-mode reader for a virtual Render endpoint (Windows playback side).
// The engine polls the shared SPSC ring in its render callback; the
// notification event fires only on connect/disconnect and telemetry
// changes, never per block. DriftResampler tracks ring fill against the
// target fill so independent driver/engine clocks converge.
class VirtualCapture {
public:
    explicit VirtualCapture(VirtualRingBuffer& ring) : ring_(ring) {}

    bool open(const EndpointDescriptor& desc, double engineRate,
              int capacityFrames, int targetFillFrames) {
        error_.clear();
        if (!validate(desc)) {
            error_ = "Virtual endpoint rejected: bad protocol.";
            open_ = false;
            return false;
        }
        if (static_cast<Direction>(desc.direction) !=
            Direction::RenderToEngine) {
            error_ = "Virtual endpoint rejected: wrong direction.";
            open_ = false;
            return false;
        }
        if (!isRateCompatible(engineRate,
                              static_cast<double>(desc.sampleRate))) {
            error_ = "Endpoint rate " +
                     std::to_string(desc.sampleRate) +
                     " != Engine rate " + std::to_string(engineRate);
            open_ = false;
            return false;
        }
        ring_.prepare(std::max(8, capacityFrames));
        targetFill_ = std::max(1, targetFillFrames);
        resampler_.configure(DriftResampler::Config{});
        resampler_.reset();
        ratio_ = 1.0;
        open_ = true;
        return true;
    }

    void close() noexcept { open_ = false; }
    bool isOpen() const noexcept { return open_; }
    const std::string& error() const noexcept { return error_; }
    double ratio() const noexcept { return ratio_; }
    std::uint64_t underruns() const noexcept { return ring_.underruns(); }
    std::uint64_t overruns() const noexcept { return ring_.overruns(); }
    int latencySamples() const noexcept { return targetFill_; }

    // Driver side writes into the ring (kernel timer copy in production,
    // test feed here). Returns false when the ring is full.
    bool feed(const float* left, const float* right, int frames) noexcept {
        return ring_.write(left, right, frames);
    }

    // Engine render thread: pulls up to `frames`, zero-filling the rest.
    // Returns frames served from the ring. Never blocks or allocates.
    int pull(float* left, float* right, int frames, double dtSeconds) noexcept {
        if (!open_ || frames <= 0) return 0;
        const int served = ring_.read(left, right, frames);
        const double fillError =
            static_cast<double>(ring_.available()) -
            static_cast<double>(targetFill_);
        ratio_ = resampler_.updateRatio(
            fillError, static_cast<double>(std::max(1, targetFill_)),
            dtSeconds);
        return served;
    }

private:
    VirtualRingBuffer& ring_;
    DriftResampler resampler_;
    double ratio_ = 1.0;
    int targetFill_ = 0;
    bool open_ = false;
    std::string error_;
};

// User-mode writer for a virtual Capture endpoint (Windows recording side).
// Pushes the engine mix into the shared ring; OBS/Discord read it via
// audiodg.exe. DriftResampler tracks fill so the engine clock converges
// with the driver's kernel timer.
class VirtualPlayback {
public:
    explicit VirtualPlayback(VirtualRingBuffer& ring) : ring_(ring) {}

    bool open(const EndpointDescriptor& desc, double engineRate,
              int capacityFrames, int targetFillFrames) {
        error_.clear();
        if (!validate(desc)) {
            error_ = "Virtual endpoint rejected: bad protocol.";
            open_ = false;
            return false;
        }
        if (static_cast<Direction>(desc.direction) !=
            Direction::EngineToCapture) {
            error_ = "Virtual endpoint rejected: wrong direction.";
            open_ = false;
            return false;
        }
        if (!isRateCompatible(engineRate,
                              static_cast<double>(desc.sampleRate))) {
            error_ = "Endpoint rate " +
                     std::to_string(desc.sampleRate) +
                     " != Engine rate " + std::to_string(engineRate);
            open_ = false;
            return false;
        }
        ring_.prepare(std::max(8, capacityFrames));
        targetFill_ = std::max(1, targetFillFrames);
        resampler_.configure(DriftResampler::Config{});
        resampler_.reset();
        ratio_ = 1.0;
        open_ = true;
        return true;
    }

    void close() noexcept { open_ = false; }
    bool isOpen() const noexcept { return open_; }
    const std::string& error() const noexcept { return error_; }
    double ratio() const noexcept { return ratio_; }
    std::uint64_t overruns() const noexcept { return ring_.overruns(); }
    int latencySamples() const noexcept { return targetFill_; }

    // Engine render thread: pushes the mix block. Returns false on overrun.
    bool push(const float* left, const float* right, int frames,
              double dtSeconds) noexcept {
        if (!open_ || frames <= 0) return false;
        const bool ok = ring_.write(left, right, frames);
        const double fillError =
            static_cast<double>(ring_.available()) -
            static_cast<double>(targetFill_);
        ratio_ = resampler_.updateRatio(
            fillError, static_cast<double>(std::max(1, targetFill_)),
            dtSeconds);
        return ok;
    }

    // Driver side drains toward the WaveRT cyclic buffer.
    int drain(float* left, float* right, int frames) noexcept {
        return ring_.read(left, right, frames);
    }

private:
    VirtualRingBuffer& ring_;
    DriftResampler resampler_;
    double ratio_ = 1.0;
    int targetFill_ = 0;
    bool open_ = false;
    std::string error_;
};

} // namespace pluginrack::virt
