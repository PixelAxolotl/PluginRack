#include "pluginrack/Meters.h"

#include <algorithm>
#include <cmath>

namespace pluginrack {

namespace {

constexpr float kMaxMeterPeak = 8.0f; // +18 dB; the fader allows +12 dB.

float clampPeak(float peak) noexcept {
    if (!std::isfinite(peak)) {
        return 0.0f;
    }
    return std::clamp(peak, 0.0f, kMaxMeterPeak);
}

void raise(std::atomic<float>& slot, float clamped) noexcept {
    const float current = slot.load(std::memory_order_relaxed);
    if (clamped > current) {
        slot.store(clamped, std::memory_order_relaxed);
    }
}

} // namespace

MeterBank::MeterBank(MeterBank&& other) noexcept {
    transfer(channelPeaks_, other.channelPeaks_);
    transfer(outputPeaks_, other.outputPeaks_);
}

MeterBank& MeterBank::operator=(MeterBank&& other) noexcept {
    if (this != &other) {
        transfer(channelPeaks_, other.channelPeaks_);
        transfer(outputPeaks_, other.outputPeaks_);
    }
    return *this;
}

void MeterBank::observeChannel(std::uint32_t id, float peak) noexcept {
    observe(channelPeaks_, id, peak);
}

void MeterBank::observeOutput(std::uint32_t id, float peak) noexcept {
    observe(outputPeaks_, id, peak);
}

void MeterBank::observeChannelRealtime(std::uint32_t id, float peak) noexcept {
    observeRealtime(channelPeaks_, id, peak);
}

void MeterBank::observeOutputRealtime(std::uint32_t id, float peak) noexcept {
    observeRealtime(outputPeaks_, id, peak);
}

void MeterBank::advance(int numFrames, double sampleRate) noexcept {
    if (numFrames <= 0 || sampleRate <= 0.0) {
        return;
    }
    const float multiplier =
        static_cast<float>(std::pow(10.0, -static_cast<double>(numFrames) / sampleRate));
    decay(channelPeaks_, multiplier);
    decay(outputPeaks_, multiplier);
}

float MeterBank::channelPeak(std::uint32_t id) const noexcept {
    return peakOf(channelPeaks_, id);
}

float MeterBank::outputPeak(std::uint32_t id) const noexcept {
    return peakOf(outputPeaks_, id);
}

void MeterBank::setTargets(const std::uint32_t* channelIds, std::size_t channelCount,
                           const std::uint32_t* outputIds, std::size_t outputCount) noexcept {
    clear(channelPeaks_);
    clear(outputPeaks_);
    for (std::size_t i = 0; i < channelCount; ++i) {
        observe(channelPeaks_, channelIds[i], 0.0f);
    }
    for (std::size_t i = 0; i < outputCount; ++i) {
        observe(outputPeaks_, outputIds[i], 0.0f);
    }
}

void MeterBank::reset() noexcept {
    clear(channelPeaks_);
    clear(outputPeaks_);
}

// Registered slots always occupy a prefix of the table: observe() takes the
// first inactive slot and single ids are never erased, so a scan can stop at
// the first inactive slot. Only the message thread registers, which is what
// keeps that prefix invariant true.
MeterBank::Slot* MeterBank::findSlot(Table& table, std::uint32_t id) noexcept {
    for (auto& slot : table) {
        if (!slot.active.load(std::memory_order_acquire)) {
            return nullptr;
        }
        if (slot.id.load(std::memory_order_relaxed) == id) {
            return &slot;
        }
    }
    return nullptr;
}

const MeterBank::Slot* MeterBank::findSlot(const Table& table, std::uint32_t id) noexcept {
    for (const auto& slot : table) {
        if (!slot.active.load(std::memory_order_acquire)) {
            return nullptr;
        }
        if (slot.id.load(std::memory_order_relaxed) == id) {
            return &slot;
        }
    }
    return nullptr;
}

void MeterBank::observe(Table& table, std::uint32_t id, float peak) noexcept {
    const float clamped = clampPeak(peak);
    if (auto* slot = findSlot(table, id)) {
        raise(slot->peak, clamped);
        return;
    }
    for (auto& slot : table) {
        if (slot.active.load(std::memory_order_relaxed)) {
            continue;
        }
        slot.peak.store(clamped, std::memory_order_relaxed);
        slot.id.store(id, std::memory_order_relaxed);
        // Published last: a reader that sees active also sees the id and peak.
        slot.active.store(true, std::memory_order_release);
        return;
    }
}

void MeterBank::observeRealtime(Table& table, std::uint32_t id, float peak) noexcept {
    auto* slot = findSlot(table, id);
    if (slot == nullptr) {
        return;
    }
    raise(slot->peak, clampPeak(peak));
}

void MeterBank::decay(Table& table, float multiplier) noexcept {
    for (auto& slot : table) {
        if (!slot.active.load(std::memory_order_relaxed)) {
            continue;
        }
        const float decayed = slot.peak.load(std::memory_order_relaxed) * multiplier;
        slot.peak.store(decayed, std::memory_order_relaxed);
    }
}

float MeterBank::peakOf(const Table& table, std::uint32_t id) noexcept {
    const auto* slot = findSlot(table, id);
    return slot == nullptr ? 0.0f : slot->peak.load(std::memory_order_relaxed);
}

void MeterBank::clear(Table& table) noexcept {
    for (auto& slot : table) {
        slot.active.store(false, std::memory_order_release);
        slot.peak.store(0.0f, std::memory_order_relaxed);
    }
}

// Registered slots form a prefix in both tables, so the copy keeps their order
// and leaves the source empty.
void MeterBank::transfer(Table& to, Table& from) noexcept {
    clear(to);
    for (std::size_t i = 0; i < kMaxSlots; ++i) {
        auto& source = from[i];
        if (!source.active.load(std::memory_order_relaxed)) {
            break;
        }
        auto& target = to[i];
        target.peak.store(source.peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
        target.id.store(source.id.load(std::memory_order_relaxed), std::memory_order_relaxed);
        target.active.store(true, std::memory_order_release);
    }
    clear(from);
}

} // namespace pluginrack
