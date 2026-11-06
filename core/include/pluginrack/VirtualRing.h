#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace pluginrack::virt {

// Single-producer single-consumer stereo float ring shared between the
// kernel driver client and the engine render thread. The engine polls
// head/tail in its render callback; the notification event fires only on
// connect/disconnect and telemetry changes, never per block.
//
// Not thread-safe beyond one writer and one reader. All methods are
// noexcept and allocation-free after prepare() so the audio thread can
// call read()/write() directly.
class VirtualRingBuffer {
public:
    void prepare(int capacityFrames) noexcept {
        capacity_ = std::max(0, capacityFrames);
        left_.assign(static_cast<std::size_t>(capacity_), 0.0f);
        right_.assign(static_cast<std::size_t>(capacity_), 0.0f);
        writePos_ = 0;
        readPos_ = 0;
        used_ = 0;
        underruns_ = 0;
        overruns_ = 0;
        connected_ = true;
    }

    int capacity() const noexcept { return capacity_; }
    int available() const noexcept { return used_; }
    std::uint64_t underruns() const noexcept { return underruns_; }
    std::uint64_t overruns() const noexcept { return overruns_; }

    // Single engine client claims the slot via IOCTL; a second opener is
    // rejected while clientConnected() is set.
    bool claimClient() noexcept {
        bool expected = false;
        return clientConnected_.compare_exchange_strong(expected, true);
    }
    void releaseClient() noexcept { clientConnected_ = false; }
    bool clientConnected() const noexcept { return clientConnected_; }

    // Driver disconnect: reads go silent until the next write reconnects.
    void disconnect() noexcept {
        connected_ = false;
        used_ = 0;
        readPos_ = writePos_;
    }
    bool connected() const noexcept { return connected_; }

    // Writer (driver side). Returns false and counts an overrun when full.
    bool write(const float* left, const float* right, int frames) noexcept {
        if (capacity_ <= 0 || frames <= 0) return false;
        if (used_ + frames > capacity_) {
            ++overruns_;
            return false;
        }
        for (int i = 0; i < frames; ++i) {
            const int pos = (writePos_ + i) % capacity_;
            left_[static_cast<std::size_t>(pos)] = left != nullptr ? left[i] : 0.0f;
            right_[static_cast<std::size_t>(pos)] = right != nullptr ? right[i] : 0.0f;
        }
        writePos_ = (writePos_ + frames) % capacity_;
        used_ += frames;
        connected_ = true;
        return true;
    }

    // Reader (engine render thread). Short reads are zero-filled; an empty
    // read counts one underrun. Returns frames actually served from the ring.
    int read(float* left, float* right, int frames) noexcept {
        if (frames <= 0) return 0;
        const int served = std::min(frames, used_);
        for (int i = 0; i < served; ++i) {
            const int pos = (readPos_ + i) % (capacity_ > 0 ? capacity_ : 1);
            if (left != nullptr) left[i] = capacity_ > 0 ? left_[static_cast<std::size_t>(pos)] : 0.0f;
            if (right != nullptr) right[i] = capacity_ > 0 ? right_[static_cast<std::size_t>(pos)] : 0.0f;
        }
        for (int i = served; i < frames; ++i) {
            if (left != nullptr) left[i] = 0.0f;
            if (right != nullptr) right[i] = 0.0f;
        }
        if (served < frames) ++underruns_;
        if (capacity_ > 0) readPos_ = (readPos_ + served) % capacity_;
        used_ -= served;
        return served;
    }

private:
    int capacity_ = 0;
    std::vector<float> left_;
    std::vector<float> right_;
    int writePos_ = 0;
    int readPos_ = 0;
    int used_ = 0;
    std::uint64_t underruns_ = 0;
    std::uint64_t overruns_ = 0;
    bool connected_ = true;
    std::atomic<bool> clientConnected_{false};
};

} // namespace pluginrack::virt
