# PluginRack Latency Reduction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`-`) syntax for tracking.

**Goal:** Reduce end-to-end input-to-output latency while preserving stable audio playback, clear device errors, and existing routing/plugin behavior.

**Architecture:** Keep the separate input/output stream model initially, but make it real-time safe (no allocations/resizes on the audio thread), bounded and observable (FIFOs sized to a small multiple of the device block with atomic overflow/underflow counters), and configurable (app-level buffer size passed through `AudioDeviceSetup`). Add shared full-duplex clock support where input and output share one device, and report device vs. plugin vs. FIFO contributions separately. Label the estimate as approximate.

**Tech Stack:** C++20, JUCE 9 (app), MSVC `/W4 /WX`, no new dependencies. Core library untouched except where noted.

**Spec:** original design doc removed from repo root; plan is self-contained.

## Global Constraints

- C++20, MSVC `/W4 /WX` clean (warnings are errors).
- No new dependencies (JUCE only).
- `MixerEngine::render` and all audio callbacks must be `noexcept`, allocation-free, lock-free on the audio thread, no filesystem or UI calls.
- Preserve routing, mute/solo, plugin, meter, and send-gain behavior.
- Conventional Commit messages, no AI attribution trailers.
- Never break `ctest --preset dev` (44 tests green at start) or `pluginrack_app` build.
- Do not force a smaller buffer if it causes underruns; do not claim lower measured latency from the UI estimate alone.

## Review Focus

- A rejected buffer size must surface a visible error and leave the prior configuration intact — not silently fall back. (Task 3)
- An oversized callback block must fail safely (silence that block) without corrupting channel/output buffers. (Task 1)
- Underrun/overrun counters must increment exactly once per affected block and never allocate. (Task 2)
- Full-duplex path must invoke `AudioEngine::render()` exactly once per output block; no duplicate renders. (Task 5)
- Latency report must remain valid (and clearly invalid) when no clock output exists; smoothing must not hide an active underrun/overrun. (Task 4)

---

### Task 1: Remove real-time allocations and unsafe resizing

**Files:**
- Modify: `app/src/AudioEngine.h` (~lines 18-35, 37-133)
- Modify: `app/src/AudioEngine.cpp` (render path lines 37-133; `prepareToPlay` lines 18-35)
- Modify: `app/src/DeviceStream.h` / `app/src/DeviceStream.cpp` (`InputCapture::pull` lines 100-129, `OutputPlayback` lines 233-290)
- Modify: `app/src/MainComponent.cpp` (wire `prepareToPlay` into clock setup)

**Interfaces:**
- Consumes: existing `AudioEngine::prepareToPlay(const juce::AudioSourceChannelInfo&)` (currently dead).
- Produces: `AudioEngine::prepareToPlay(int maxBlockFrames)` (new signature) allocating channel/output buffers once; `InputCapture::pull(dest, frames)` no longer resizes `dest`.

- [ ] **Step 1: Implement `prepareToPlay(int maxBlockFrames)`.** Allocate `channel->buffer` and `output->buffer` to `(2, maxBlockFrames)` for every channel/output; store `maxBlockFrames_`. Call it from `MainComponent` after the clock output opens and on device change, passing the actual device buffer size (clamped to a sane max, e.g. 4096).
- [ ] **Step 2: Remove `setSize` from render.** In `AudioEngine::render`, replace `buffer.setSize(...)` (lines 55, 64) with: if `frames > maxBlockFrames_`, push silence for that block, increment a new `oversizedBlockCount_` atomic, and skip that channel/output. No allocation.
- [ ] **Step 3: Remove resize from `InputCapture::pull`.** Require `dest` already sized to `(2, >= frames)`; return false if not. Clear only the used range.
- [ ] **Step 4: Remove the silence-buffer allocation.** Replace `juce::AudioBuffer<float> silence(2, frames)` (line 44) with a preallocated member `silenceBuffer_` sized in `prepareToPlay`, reused.
- [ ] **Step 5: Replace the blocking lock in `getLatencyReport`.** Use `ScopedTryLock`; if not acquired, return a report with `valid = false`. Never block the UI thread on the audio lock.
- [ ] **Step 6: Verify.** Build `pluginrack_app` clean (`/W4 /WX`). Run `ctest --preset dev` (all pass). Manual: play audio, change buffer size, confirm no dropouts. Commit: `fix: remove real-time allocations and resizing from audio path`.

---

### Task 2: Bounded FIFOs with overflow/underflow telemetry

**Files:**
- Modify: `app/src/DeviceStream.h` (add atomics + capacity helper)
- Modify: `app/src/DeviceStream.cpp` (`InputCapture` lines 78-82, 100-159; `OutputPlayback` lines 211-216, 233-290)

**Interfaces:**
- Produces: `InputCapture::inputOverruns` / `inputDroppedFrames` (atomic<uint64>); `OutputPlayback::outputUnderruns` / `outputDroppedFrames` (atomic<uint64>); `getFifoOccupancy()` returning 0..1.

- [ ] **Step 1: Size FIFOs to a small multiple of the device block.** Replace `sampleRate * 2.0` (lines 80-82, 214-216) with `juce::jmax(1, deviceBufferSize * 4)`. Store `deviceBufferSize_`.
- [ ] **Step 2: Add atomics.** Increment in the contended-drop path (lines 132-133) and in `OutputPlayback` when `push` cannot fit.
- [ ] **Step 3: Define explicit overflow/underflow behavior.** Input overflow: drop the newest incoming block and increment counters. Output underflow: write silence for the missing frames and increment counters. Document in comments.
- [ ] **Step 4: Allocation-free callback.** Verify no `new`/`setSize`/lock-contention allocation in either callback.
- [ ] **Step 5: Expose counters to `AudioEngine`.** Add `AudioEngine::getFifoDiagnostics()` returning the four atomics + occupancy, aggregated across all captures/playbacks.
- [ ] **Step 6: Verify and commit.** Build clean; commit `feat: bounded FIFOs with overflow/underflow telemetry`.

---

### Task 3: Configurable device buffer size

**Files:**
- Modify: `app/src/DeviceStream.h` / `.cpp` (both `open()` functions)
- Modify: `app/src/AudioEngine.h` / `.cpp` (`currentBufferSize`, `setInputDevice`, `setOutputDevice`)
- Modify: `app/src/DeviceControl.h` / `.cpp` or `MainComponent` audio menu for the selector
- Modify: `app/src/MainComponent.cpp` (audio menu + clock description)

**Interfaces:**
- Consumes: JUCE `AudioDeviceManager::AudioDeviceSetup` with `bufferSize`.
- Produces: `DeviceStream::open(..., int requestedBufferSize, juce::String& errorOut)` storing `requestedBufferSize_` and `actualBufferSize_`; `AudioEngine::setDeviceBufferSize(int)` returning `juce::String` (empty = success); `getClockDescription()` reports actual.

- [ ] **Step 1: Pass `AudioDeviceSetup` in both opens.** Build a setup with `bufferSize = requestedBufferSize` (0 = driver default) and call `manager.setAudioDeviceSetup(setup, true)`, capturing the returned error string. Store actual via `dev->getCurrentBufferSizeSamples()`.
- [ ] **Step 2: Preserve driver default when 0.** If `requestedBufferSize == 0`, pass the device's current size (no change).
- [ ] **Step 3: Surface rejection.** If `setAudioDeviceSetup` returns an error, restore the previous device setup and propagate the error string; do not leave a half-open device.
- [ ] **Step 4: AudioEngine plumbing.** Add `requestedBufferSize_` member (default 0); `setDeviceBufferSize(int)` reopens input/output devices with the new size and returns the first error. Update `currentBufferSize` to the actual.
- [ ] **Step 5: UI.** Add a buffer-size selector (32/64/128/256/0=Auto) to the Audio menu in `MainComponent` (lines 53-66) or `DeviceControl`. Show actual active size in `getClockDescription` (line 614-615) and latency label tooltip.
- [ ] **Step 6: Verify.** Build clean; manual test at 48 kHz with 256/128/64/32 where supported; unsupported size shows an error. Commit `feat: configurable device buffer size`.

---

### Task 4: Latency report refactor + diagnostics UI

**Files:**
- Modify: `app/src/AudioEngine.h` (LatencyReport struct lines 59-65)
- Modify: `app/src/AudioEngine.cpp` (`getLatencyReport` lines 651-692)
- Modify: `app/src/PluginHost.h` / `.cpp` (cache latency)
- Modify: `app/src/MainComponent.cpp` (latency label + menu breakdown lines 56-101)

**Interfaces:**
- Produces: `LatencyReport` with fields `inputDeviceMs`, `outputDeviceMs`, `pluginMs`, `fifoMs`, `totalMs`, `inputBufferSize`, `outputBufferSize`, `sampleRate`, `underruns`, `overruns`, `valid`, `approximate` (true). Per-insert max latency reported.

- [ ] **Step 1: Extend `LatencyReport`.** Add the fields above; keep `valid` semantics (clock output exists).
- [ ] **Step 2: Compute device contributions separately.** Input = `deviceInputLatency + inputBufferSize` (DeviceStream line 114); output = `deviceOutputLatency + outputBufferSize` (line 253). Report both.
- [ ] **Step 3: Plugin contribution.** Sum inserts of the clock output's channel only (existing behavior at line 655-670); also report the single highest-latency insert id+ms. Cache `PluginHost::getLatencySamples()` result (invalidate on add/remove/bypass) to avoid the blocking lock.
- [ ] **Step 4: FIFO contribution.** Add estimated FIFO depth in ms from Task 2 occupancy.
- [ ] **Step 5: UI.** Label the value as approximate ("~X ms (est.)"). Add underrun/overrun warning that overrides smoothing (never hide an active counter). Menu breakdown lists input device, plugin, output device, FIFO.
- [ ] **Step 6: Verify and commit.** Build clean; commit `feat: separate and label latency contributions`.

---

### Task 5: Shared full-duplex device support

**Files:**
- Modify: `app/src/DeviceStream.h` / `.cpp` (add shared manager path)
- Modify: `app/src/AudioEngine.h` / `.cpp` (clock setup, render hook)

**Interfaces:**
- Produces: `AudioEngine::openFullDuplexIfPossible(inputDevice, outputDevice)` returning bool + error; `isFullDuplex()`.

- [ ] **Step 1: Assess same-hardware.** Add a helper comparing input and output device names/ids; only attempt full-duplex when they match (or user selects one device for both).
- [ ] **Step 2: Shared manager path.** When full-duplex, use one `AudioDeviceManager` with `initialise(2, 2, ...)`, one callback that calls `renderHook` exactly once. Remove the separate output callback's render call in that mode.
- [ ] **Step 3: Mismatched rates/devices.** If sample rates differ by >0.5 Hz, reject full-duplex with an explicit error and keep separate streams. Independently clocked devices: document as unsupported for full-duplex, fall back.
- [ ] **Step 4: Ensure single render.** Guard `render()` invocation with the clock flag; debug-assert no duplicate.
- [ ] **Step 5: Keep separate-device path.** Do not remove it; full-duplex is opt-in/automatic only when verified.
- [ ] **Step 6: Verify and commit.** Build clean; manual test sustained playback with one device; commit `feat: shared full-duplex device clock`.

---

### Task 6: Plugin latency handling + low-latency mode

**Files:**
- Modify: `app/src/AudioEngine.h` / `.cpp` (insert latency, bypass)
- Modify: `app/src/ChannelStrip.cpp` (display highest-latency insert)
- Modify: `core/src/InsertChain.cpp` / `.h` (report latency if plugin exposes it — optional)

**Interfaces:**
- Produces: `InsertSlot::latencySamples` cached; `AudioEngine::getHighestLatencyInsert(channelId)`; bypass toggle already exists.

- [ ] **Step 1: Cache plugin-reported latency.** After `prepare`, read `plugin->getLatencySamples()` and store on the insert slot. Refresh on bypass change.
- [ ] **Step 2: Display.** Show highest-latency insert name + ms in diagnostics (Task 4 UI). Bypassing a latency-heavy plugin updates the reported total when the plugin reports a change.
- [ ] **Step 3: Low-latency mode (optional, gated).** Add a per-channel "low-latency" flag that bypasses inserts reporting non-zero latency; document. Do not enable delay compensation without tests (stop condition).
- [ ] **Step 4: Verify and commit.** Build clean; manual test with a latency-reporting plugin; commit `feat: plugin latency reporting and low-latency mode`.

---

### Task 7: Validation and ship

**Files:** none (verification only).

- [ ] **Step 1: Full build + tests.** `cmake --build --preset dev --target pluginrack_app`, then `ctest --preset dev`. Expected: clean, all pass.
- [ ] **Step 2: Manual matrix.** Test at 48 kHz with 256/128/64/32; sustained audio: one input+output, multiple outputs, several inserts, a plugin reporting non-zero latency, separate devices. Confirm no dropouts, callback allocations, duplicate renders, unbounded FIFO growth.
- [ ] **Step 3: Physical loopback.** Compare estimated vs measured round-trip; document difference. Do not claim lower measured latency from estimate alone.
- [ ] **Step 4: Ship.** Copy exe to repo-root `PluginRack.exe` (gitignored). Push only if all pass.
