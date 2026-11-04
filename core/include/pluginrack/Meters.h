#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pluginrack {

// Peak meter store. Peaks are linear amplitude, post-fader and post-send.
// observe*() keeps the larger of the stored and new peak; advance() applies
// a 20 dB/s release. observe*Realtime() only updates ids registered by
// observe*(), so audio-thread updates never allocate.
//
// The store is a fixed table of atomic slots: the message thread may register
// ids, re-target the bank (setTargets) or reset it while the audio thread
// reads, because no slot is ever allocated or freed. Ids beyond kMaxSlots are
// left unmetered (channelPeak/outputPeak report zero for them).
class MeterBank {
public:
    MeterBank() noexcept = default;
    MeterBank(const MeterBank&) = delete;
    MeterBank& operator=(const MeterBank&) = delete;
    // Moves the registered ids; the source is emptied. Copies are refused so a
    // shared bank cannot be duplicated by accident.
    MeterBank(MeterBank&& other) noexcept;
    MeterBank& operator=(MeterBank&& other) noexcept;
    ~MeterBank() = default;

    void observeChannel(std::uint32_t id, float peak) noexcept;
    void observeOutput(std::uint32_t id, float peak) noexcept;
    void observeChannelRealtime(std::uint32_t id, float peak) noexcept;
    void observeOutputRealtime(std::uint32_t id, float peak) noexcept;
    void advance(int numFrames, double sampleRate) noexcept; // 20 dB/s release
    float channelPeak(std::uint32_t id) const noexcept;      // 0.0f when unknown
    float outputPeak(std::uint32_t id) const noexcept;       // 0.0f when unknown
    // Replaces the metered id set, registering each id at zero. Safe to call
    // while the audio thread reads the bank.
    void setTargets(const std::uint32_t* channelIds, std::size_t channelCount,
                    const std::uint32_t* outputIds, std::size_t outputCount) noexcept;
    void reset() noexcept;

private:
    static constexpr std::size_t kMaxSlots = 64;

    struct Slot {
        std::atomic<std::uint32_t> id{0};
        std::atomic<float> peak{0.0f};
        std::atomic<bool> active{false};
    };

    using Table = Slot[kMaxSlots];

    static Slot* findSlot(Table& table, std::uint32_t id) noexcept;
    static const Slot* findSlot(const Table& table, std::uint32_t id) noexcept;
    static void observe(Table& table, std::uint32_t id, float peak) noexcept;
    static void observeRealtime(Table& table, std::uint32_t id, float peak) noexcept;
    static void decay(Table& table, float multiplier) noexcept;
    static float peakOf(const Table& table, std::uint32_t id) noexcept;
    static void clear(Table& table) noexcept;
    static void transfer(Table& to, Table& from) noexcept;

    Table channelPeaks_;
    Table outputPeaks_;
};

} // namespace pluginrack
