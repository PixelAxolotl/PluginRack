#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace pluginrack {

// Guided search for the lowest buffer size that still runs cleanly: step one
// notch lower, watch for a full dwell, then continue or retreat. The candidate
// list is frozen in start() so a run never allocates while it is live and can
// never oscillate or settle below the buffer it began with.
class LatencyProfile {
public:
    enum class Action {
        Done,      // finished successfully; the current step is the settled result
        Apply,     // apply nextBufferSamples at the current step
        Hold,      // keep observing; nothing to change yet
        Revert     // go back to the starting buffer; the run failed
    };

    struct Config {
        std::vector<int> bufferSizes{32, 64, 128, 256, 480, 512, 1024, 2048, 4096};
        int dwellMs = 45000;
        int maxSteps = 6;
    };

    struct Status {
        bool active = false;
        bool finished = false;
        bool failed = false;
        int stepIndex = 0;         // 0 = not started / the starting step
        int totalSteps = 0;
        int startBuffer = 0;
        int currentBuffer = 0;
        int remainingMs = 0;       // dwell time left for the current step
    };

    void start(int startBufferSamples, const Config& config) {
        candidates_.clear();
        status_ = Status{};
        accumulatedMs_ = 0.0;
        pendingRevert_ = false;
        lastAction_ = Action::Done;

        dwellMs_ = std::max(0, config.dwellMs);
        maxSteps_ = std::max(0, config.maxSteps);

        status_.startBuffer = startBufferSamples;
        status_.currentBuffer = startBufferSamples;
        status_.active = true;
        requestedBuffer_ = startBufferSamples;

        for (const int size : config.bufferSizes) {
            if (size > 0 && size < startBufferSamples) {
                candidates_.push_back(size);
            }
        }
        std::sort(candidates_.begin(), candidates_.end(), [](int a, int b) { return a > b; });
        if (candidates_.size() > static_cast<std::size_t>(maxSteps_)) {
            candidates_.resize(static_cast<std::size_t>(maxSteps_));
        }

        status_.totalSteps = static_cast<int>(candidates_.size());
        if (candidates_.empty()) {
            // Nothing lower to try, so the starting buffer is already the answer.
            status_.active = false;
            status_.finished = true;
        }
    }

    void cancel() {
        const int startBuffer = status_.startBuffer;
        candidates_.clear();
        status_ = Status{};
        status_.startBuffer = startBuffer;
        status_.currentBuffer = startBuffer;
        accumulatedMs_ = 0.0;
        pendingRevert_ = false;
        requestedBuffer_ = startBuffer;
        lastAction_ = Action::Done;
    }

    // Call on a timer (e.g. once per second) with the number of NEW audio events
    // (underruns / render misses) observed since the previous call.
    // elapsedMs: milliseconds since the previous call.
    Action tick(int eventsSinceLastTick, double elapsedMs) noexcept {
        if (pendingRevert_) {
            // onApplied() already knows the run is lost, but the caller still
            // needs one explicit Revert to put the device back.
            pendingRevert_ = false;
            lastAction_ = Action::Revert;
            return Action::Revert;
        }

        if (!status_.active || status_.finished) {
            return Action::Done;
        }

        if (status_.stepIndex == 0) {
            // Nothing has been asked of the device yet, so this tick only starts
            // the run; events seen while still on the starting buffer are ignored.
            return applyStep(0);
        }

        if (eventsSinceLastTick > 0) {
            return fail();
        }

        accumulatedMs_ += (elapsedMs > 0.0) ? elapsedMs : 0.0;  // also drops NaN

        const double dwell = static_cast<double>(dwellMs_);
        const int remaining = dwellMs_ - static_cast<int>(std::min(accumulatedMs_, dwell));
        status_.remainingMs = (remaining > 0) ? remaining : 0;

        if (accumulatedMs_ < dwell) {
            lastAction_ = Action::Hold;
            return Action::Hold;
        }

        accumulatedMs_ = 0.0;
        status_.remainingMs = 0;

        if (status_.stepIndex >= status_.totalSteps || status_.stepIndex >= maxSteps_) {
            // Running out of steps is success, not failure: whatever we already
            // proved is the settled result.
            status_.finished = true;
            status_.active = false;
            lastAction_ = Action::Done;
            return Action::Done;
        }

        return applyStep(status_.stepIndex);
    }

    // The engine reports the buffer the device actually opened with, which may
    // differ from the request. Call after applying a step.
    void onApplied(int actualBufferSamples, bool openFailed) noexcept {
        if (pendingRevert_ || !status_.active || status_.finished) {
            return;
        }

        if (openFailed) {
            // Mark the run dead now but let the next tick emit Revert, so the
            // caller always observes the action before it reads the status.
            status_.failed = true;
            status_.finished = true;
            status_.active = false;
            status_.currentBuffer = status_.startBuffer;
            status_.remainingMs = 0;
            accumulatedMs_ = 0.0;
            pendingRevert_ = true;
            return;
        }

        if (actualBufferSamples == requestedBuffer_) {
            return;
        }

        // The device opened somewhere else; that is the buffer we are now
        // observing, so the dwell has to start over.
        status_.currentBuffer = actualBufferSamples;
        accumulatedMs_ = 0.0;
        status_.remainingMs = dwellMs_;
    }

    Status status() const noexcept {
        return status_;
    }

    Action lastAction() const noexcept {
        return lastAction_;
    }

private:
    Action applyStep(int index) noexcept {
        const int buffer = candidates_[static_cast<std::size_t>(index)];
        status_.stepIndex = index + 1;
        status_.currentBuffer = buffer;
        requestedBuffer_ = buffer;
        accumulatedMs_ = 0.0;
        status_.remainingMs = dwellMs_;
        lastAction_ = Action::Apply;
        return Action::Apply;
    }

    Action fail() noexcept {
        status_.failed = true;
        status_.finished = true;
        status_.active = false;
        status_.currentBuffer = status_.startBuffer;
        status_.remainingMs = 0;
        accumulatedMs_ = 0.0;
        lastAction_ = Action::Revert;
        return Action::Revert;
    }

    std::vector<int> candidates_;
    Status status_;
    int dwellMs_ = 0;
    int maxSteps_ = 0;
    int requestedBuffer_ = 0;
    double accumulatedMs_ = 0.0;
    bool pendingRevert_ = false;
    Action lastAction_ = Action::Done;
};

}  // namespace pluginrack
