#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// Message-thread mirror of the parameter edits made on a slot's in-process
// instance, so a child-DSP slot can receive them without the audio thread ever
// producing into the IPC ring.
//
// AudioProcessorParameter::parameterValueChanged can fire on the plugin's UI
// thread or its audio thread, and the IPC ring is single-producer, so the
// listener must not push directly. It only records the latest value here - one
// atomic store, no allocation, no locks - and a message-thread timer drains the
// dirty entries into the ring. Repeats of the same parameter collapse, so a
// fast knob drag costs one push per drain tick instead of one per sample of
// movement.
//
// NaN marks "clean": plugin parameters are finite normalised values, so the
// sentinel can never collide with a real edit.
class PluginParamMirror {
public:
    static constexpr float kClean = std::numeric_limits<float>::quiet_NaN();

    // Message thread only; allocates. Called when the listener set changes, so
    // it never races a callback (detach and attach are both serialised by the
    // parameter's listener lock).
    void resize(std::size_t count) {
        std::vector<std::atomic<float>> fresh(count);
        for (auto& value : fresh) value.store(kClean, std::memory_order_relaxed);
        pending_.swap(fresh);
    }

    // Message thread. Forgets every pending edit: used when a state blob that
    // already contains them is pushed to the child.
    void clear() noexcept {
        for (auto& value : pending_) value.store(kClean, std::memory_order_relaxed);
    }

    // Message thread. A disarmed mirror costs the listener one atomic load and
    // nothing else, which is what an in-process slot (no child) pays.
    void setArmed(bool armed) noexcept { armed_.store(armed, std::memory_order_release); }
    bool armed() const noexcept { return armed_.load(std::memory_order_acquire); }

    std::size_t size() const noexcept { return pending_.size(); }

    // Any thread, non-blocking: record the latest value for `index`.
    void markDirty(int index, float value) noexcept {
        if (!armed_.load(std::memory_order_acquire)) return;
        const auto i = static_cast<std::size_t>(index);
        if (i >= pending_.size()) return;
        pending_[i].store(value, std::memory_order_release);
    }

    // Message thread: hand every dirty entry to `sink(index, value)` and clear
    // it. A sink that returns false (the ring was full) leaves the entry dirty
    // for the next drain, unless a newer edit already replaced the value while
    // the sink ran - the compare-exchange then keeps the newer one.
    template <typename Sink>
    std::size_t drain(Sink&& sink) {
        std::size_t drained = 0;
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            const auto value = pending_[i].exchange(kClean, std::memory_order_acq_rel);
            if (std::isnan(value)) continue;
            ++drained;
            if (!sink(static_cast<int>(i), value)) {
                float expected = kClean;
                pending_[i].compare_exchange_strong(expected, value,
                                                    std::memory_order_acq_rel);
            }
        }
        return drained;
    }

    bool empty() const noexcept {
        for (const auto& value : pending_)
            if (!std::isnan(value.load(std::memory_order_acquire))) return false;
        return true;
    }

private:
    std::vector<std::atomic<float>> pending_;
    std::atomic<bool> armed_{false};
};
