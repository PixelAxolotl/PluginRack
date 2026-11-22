# Clock Auto-Reclaim Implementation Plan

> **For the human partner:** this plan is executed natively in this session. Steps use checkbox syntax for tracking.

**Goal:** When the preferred output device (the first output) disappears and later comes back, the engine automatically reopens it and gives it the clock role back from the promoted output or the internal clock.

**Architecture:** The existing 4 Hz UI tick already runs `checkClockHealth()`. Add `maybeReclaimClock()` beside it. While no device job is running, it inspects the preferred (lowest-id) output: if that output is not the healthy clock and its named endpoint is present in the cached device enumeration, it asks `reloadDevicesAsync()` to reopen it as a normal output at the engine's current rate. A later tick promotes the reopened, live, same-rate output back to clock, demoting the current clock and stopping the internal timer. Same-rate only, mirroring `chooseClockFailover`.

**Tech stack:** C++17, JUCE, Catch2, CMake/MSVC.

**Spec:** `docs/superpowers/specs/2026-11-22-clock-auto-reclaim-design.md`

## Global constraints

- All reclaim watch state is touched only on the message thread, or inside the device job while `openingDevices_` is true. `maybeReclaimClock()` returns immediately when `isOpeningDevices()`.
- Named endpoints only: skip empty names, `isSilentDeviceName`, and `isSystemDefaultName`.
- Never reclaim at a different sample rate; the engine keeps the fallback playing when the rate does not match.
- At most one reopen attempt per 5 s, at most 3 per plug-in episode. Reset on absence or success.
- No new dependencies. No commit unless the user asks.
- After source changes run `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`; the repo-root `PluginRack.exe` must be refreshed.
- Existing unrelated working-tree edits are user work: do not touch or revert them.

## Review focus

- The demoted clock must be deactivated before the preferred output is activated, and must get normal FIFO headroom back (`setTargetFillBlocks(4)`).
- The preferred output must get `setTargetFillBlocks(0)`, the same render hook/lambda, and `setRenderActive(true)`.
- `clockOutputId`, `publishRenderOutputsLocked()`, `stopInternalClock()`, and the strip error must all be updated together.
- Do not attempt reclaim while a device job is opening; do not fight a manual Reload.
- A rate mismatch or dead stream must leave the fallback untouched.

---

### Task 1: Same-rate reclaim eligibility helper

**Files:**
- Modify: `core/include/pluginrack/ClockFailover.h`
- Test: `tests/test_clock_failover.cpp`

**Interfaces:**
- Produces: `inline bool pluginrack::canReclaimClock(bool deviceOpen, bool deviceLive, int deviceSampleRate, int engineSampleRate) noexcept`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_clock_failover.cpp`:

```cpp
TEST_CASE("only a live same-rate output can reclaim the clock", "[clock][failover]") {
    using pluginrack::canReclaimClock;
    CHECK(canReclaimClock(true, true, 48000, 48000));
    CHECK_FALSE(canReclaimClock(false, true, 48000, 48000));
    CHECK_FALSE(canReclaimClock(true, false, 48000, 48000));
    CHECK_FALSE(canReclaimClock(true, true, 44100, 48000));
    CHECK_FALSE(canReclaimClock(true, true, 0, 48000));
    CHECK_FALSE(canReclaimClock(true, true, 48000, 0));
}
```

- [ ] **Step 2: Run the test and confirm it fails**

Run: `cmake --build build/dev --target pluginrack_tests`
Expected: compile error, `canReclaimClock` is not a member of `pluginrack`.

- [ ] **Step 3: Implement the helper**

In `core/include/pluginrack/ClockFailover.h`, before the closing namespace:

```cpp
// A recovered output may take the clock role back only when it is open, its
// callback is live, and it negotiated the rate the session already runs at.
// Reclaiming at a different rate would re-prime every FIFO, which is exactly
// what chooseClockFailover refuses to do.
inline bool canReclaimClock(bool deviceOpen, bool deviceLive,
                            int deviceSampleRate, int engineSampleRate) noexcept {
    if (!deviceOpen || !deviceLive) return false;
    if (deviceSampleRate <= 0 || engineSampleRate <= 0) return false;
    return deviceSampleRate == engineSampleRate;
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build/dev --target pluginrack_tests`
Then: `.\build\dev\tests\pluginrack_tests.exe "[clock],[failover]"`
Expected: all clock/failover cases pass.

---

### Task 2: `maybeReclaimClock()` and tick wiring

**Files:**
- Modify: `app/src/AudioEngine.h`
- Modify: `app/src/AudioEngine.cpp`
- Modify: `app/src/MainComponent.cpp`
- Test: `tests/test_underrun_paths.cpp`

**Interfaces:**
- Produces: `bool AudioEngine::maybeReclaimClock();`
- Private state: `pluginrack::ReclaimGate reclaimGate_;` (message-thread only; core tests cover the window, cap and resets).
- Constants: `constexpr std::uint64_t kReclaimRetryMs = 5000; constexpr int kReclaimMaxAttempts = 3;`

- [ ] **Step 1: Write the failing source-contract test**

Extend the existing `clock loss promotes or falls back instead of ending the session` test in `tests/test_underrun_paths.cpp`:

```cpp
    // A recovered first output reclaims the clock from a promoted output or
    // the internal timer, at the same rate only.
    const auto reclaim = functionBody(engine, "bool AudioEngine::maybeReclaimClock");
    CHECK(contains(reclaim, "isOpeningDevices()"));
    CHECK(contains(reclaim, "getAvailableOutputDevices()"));
    CHECK(contains(reclaim, "pluginrack::canReclaimClock"));
    CHECK(contains(reclaim, "reloadDevicesAsync"));
    CHECK(contains(reclaim, "setRenderActive(false)"));
    CHECK(contains(reclaim, "setRenderActive(true)"));
    CHECK(contains(reclaim, "publishRenderOutputsLocked()"));
    CHECK(contains(reclaim, "stopInternalClock()"));
    CHECK(contains(reclaim, "Clock reclaimed by"));
    CHECK(contains(readSource("app/src/MainComponent.cpp"), "maybeReclaimClock()"));
```

- [ ] **Step 2: Run the test and confirm it fails**

Run: `cmake --build build/dev --target pluginrack_tests`
Then: `.\build\dev\tests\pluginrack_tests.exe "[clock],[failover]"`
Expected: FAIL on the new `functionBody`/`contains` checks.

- [ ] **Step 3: Declare the API and state**

In `app/src/AudioEngine.h`, after `bool checkClockHealth();`:

```cpp
    // Gives the clock role back to the first output when its endpoint returns.
    // Message-thread only; no-op while a device job is running.
    bool maybeReclaimClock();
```

Beside `internalClockActive_`, add:

```cpp
    std::uint32_t reclaimLastAttemptMs_ = 0;
    int reclaimAttempts_ = 0;
    juce::String reclaimDevice_;
    bool reclaimDeviceWasPresent_ = false;
```

- [ ] **Step 4: Implement the reclaim logic**

In `app/src/AudioEngine.cpp`, next to `kClockStallGraceMs`:

```cpp
constexpr std::uint64_t kReclaimRetryMs = 5000;
constexpr int kReclaimMaxAttempts = 3;
```

After `checkClockHealth()`, add:

```cpp
bool AudioEngine::maybeReclaimClock() {
    if (isOpeningDevices()) return false;

    juce::String preferredName;
    std::uint32_t preferredId = 0;
    juce::String preferredLabel;
    bool needsReclaim = false;
    Output* preferred = nullptr;
    {
        const juce::ScopedLock lock(stateLock);
        if (outputs.empty()) return false;

        preferred = *std::min_element(
            outputs.begin(), outputs.end(),
            [](const auto& a, const auto& b) { return a->id < b->id; });
        preferredId = preferred->id;
        preferredName = preferred->outputDeviceName;
        preferredLabel = preferred->name;

        if (isSilentDeviceName(preferredName) || isSystemDefaultName(preferredName))
            return false;

        // Already the healthy clock: nothing to do.
        if (clockOutputId == preferredId && preferred->output != nullptr) {
            const auto nowMs = juce::Time::getMillisecondCounterHiRes();
            if (!preferred->output->isDeviceStopped()
                && !preferred->output->isCallbackStalled((std::uint64_t) nowMs,
                                                         kClockStallGraceMs))
                return false;
        }
        needsReclaim = true;
    }
    if (!needsReclaim) return false;

    // Presence comes from the same 5 s cache the availability poll uses.
    const auto present = getAvailableOutputDevices();
    if (present.isEmpty()) return false;
    const bool devicePresent = present.indexOf(preferredName, true) >= 0;
    if (!devicePresent) {
        reclaimAttempts_ = 0;
        reclaimDeviceWasPresent_ = false;
        return false;
    }

    if (reclaimDevice_ != preferredName) {
        reclaimAttempts_ = 0;
        reclaimDevice_ = preferredName;
    }
    reclaimDeviceWasPresent_ = true;
    if (reclaimAttempts_ >= kReclaimMaxAttempts) return false;

    const auto nowMs = (std::uint32_t) juce::Time::getMillisecondCounter();
    if (reclaimLastAttemptMs_ != 0 && nowMs - reclaimLastAttemptMs_ < kReclaimRetryMs)
        return false;

    // Try the handover first: a stream opened by a manual Reload may already
    // be live and only need the clock role.
    bool reclaimed = false;
    bool liveSameRate = false;
    if (preferred != nullptr && preferred->output != nullptr) {
        const int rate = juce::roundToInt(preferred->output->getSampleRate());
        const auto nowHi = (std::uint64_t) juce::Time::getMillisecondCounterHiRes();
        liveSameRate = pluginrack::canReclaimClock(
            true,
            !preferred->output->isDeviceStopped()
                && !preferred->output->isCallbackStalled(nowHi, kClockStallGraceMs),
            rate, (int) std::lround(currentSampleRate));
        if (liveSameRate) {
            const juce::ScopedLock lock(stateLock);
            Output* oldClock = nullptr;
            for (auto& output : outputs)
                if (output->id == clockOutputId) oldClock = output.get();

            // Deactivate first so the two streams never render in parallel.
            if (oldClock != nullptr && oldClock != preferred && oldClock->output != nullptr) {
                oldClock->output->setRenderActive(false);
                oldClock->output->setTargetFillBlocks(4);
            }
            preferred->output->setTargetFillBlocks(0);
            preferred->output->setRenderHook([this](int n) { this->render(n); });
            preferred->output->setRenderActive(true);
            clockOutputId = preferredId;
            preferred->outputError.clear();
            publishRenderOutputsLocked();
            reclaimed = true;
        }
    }

    if (reclaimed) {
        stopInternalClock();
        reclaimAttempts_ = 0;
        reclaimDeviceWasPresent_ = false;
        juce::Logger::writeToLog("audio: clock reclaimed by " + preferredName);
        return true;
    }

    // No usable stream yet: reopen the preferred endpoint, but only when the
    // endpoint is actually present so a missing device is not hammered.
    reclaimLastAttemptMs_ = nowMs;
    ++reclaimAttempts_;
    ReloadableDevice device;
    device.label = preferredLabel;
    device.deviceName = preferredName;
    device.stripId = preferredId;
    device.isInput = false;
    reloadDevicesAsync({device});
    return false;
}
```

- [ ] **Step 5: Call it from the UI tick**

In `app/src/MainComponent.cpp`, in the same non-opening branch where
`checkClockHealth()` is called:

```cpp
        if (audioEngine.checkClockHealth()) router.refreshDeviceStrips();
        if (audioEngine.maybeReclaimClock()) router.refreshDeviceStrips();
```

- [ ] **Step 6: Build and run the clock tests**

Run: `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`
Then: `.\build\dev\tests\pluginrack_tests.exe "[clock],[failover]"`
Expected: PASS, including the new source-contract checks.

---

### Task 3: Documentation and full verification

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Document reclaim in the clock section**

Add one paragraph after the existing promotion/internal-clock fallback text:

```markdown
When the first output's endpoint comes back, it reclaims the clock
automatically. The engine reopens it at the session's current sample rate and,
once its callback is live, hands the clock role back from the promoted output
or the internal clock. Reclaim only happens at the same rate; a device that
cannot run at that rate keeps the fallback playing and reports its error.
```

- [ ] **Step 2: Build with the required script**

Run: `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`
Expected: build succeeds and `PluginRack.exe` is copied to the repository root.

- [ ] **Step 3: Run the full test suite**

Run: `.\build\dev\tests\pluginrack_tests.exe`
Expected: all tests pass.

- [ ] **Step 4: Report**

Report the files changed, the build artifact timestamp, the test counts, and the residual limits (reclaim requires the endpoint to be present in the cached enumeration; a rate mismatch leaves the fallback in place; only the first output is reclaimed).
