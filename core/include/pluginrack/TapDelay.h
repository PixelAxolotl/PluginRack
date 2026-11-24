#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pluginrack {

// A tap can sit behind a chain item that adds latency. The engine aligns it
// with the strip output by delaying its signal, so the ring needs room for the
// worst case it will accept. 16384 samples is 341 ms at 48 kHz: longer plugin
// latencies are clamped rather than allowed to grow the buffers without bound.
inline constexpr int kMaxTapDelaySamples = 16384;

// Fixed-capacity stereo delay line. prepare() allocates; push() and read()
// never do, so the audio thread can use it. read() returns the block that
// arrived delaySamples ago, counting from the block just pushed, which lets a
// caller read the same tap at several compensations in one block.
class DelayRing {
public:
    void prepare(int capacity) {
        capacity_ = std::max(1, capacity);
        data_.assign(static_cast<std::size_t>(capacity_) * 2, 0.0f);
        writePos_ = 0;
    }

    void clear() noexcept {
        std::fill(data_.begin(), data_.end(), 0.0f);
        writePos_ = 0;
    }

    int capacity() const noexcept { return capacity_; }

    void push(const float* left, const float* right, int frames) noexcept {
        if (capacity_ <= 0 || frames <= 0) {
            return;
        }
        int pos = writePos_;
        for (int i = 0; i < frames; ++i) {
            data_[static_cast<std::size_t>(pos)] = left[i];
            data_[static_cast<std::size_t>(capacity_) + static_cast<std::size_t>(pos)] = right[i];
            if (++pos >= capacity_) {
                pos = 0;
            }
        }
        writePos_ = pos;
    }

    void read(int delaySamples, float* left, float* right, int frames) const noexcept {
        if (capacity_ <= 0) {
            std::fill(left, left + frames, 0.0f);
            std::fill(right, right + frames, 0.0f);
            return;
        }
        const int delay = std::clamp(delaySamples, 0, capacity_ - 1);
        int start = writePos_ - frames - delay;
        while (start < 0) {
            start += capacity_;
        }
        start %= capacity_;
        for (int i = 0; i < frames; ++i) {
            int index = start + i;
            if (index >= capacity_) {
                index -= capacity_;
            }
            left[i] = data_[static_cast<std::size_t>(index)];
            right[i] = data_[static_cast<std::size_t>(capacity_) + static_cast<std::size_t>(index)];
        }
    }

private:
    std::vector<float> data_;
    int capacity_ = 0;
    int writePos_ = 0;
};

} // namespace pluginrack
