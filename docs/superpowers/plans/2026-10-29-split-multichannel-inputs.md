# PluginRack Split Multichannel Inputs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`-`) syntax for tracking.

**Goal:** Allow one multichannel audio interface to be represented as independent logical channels that can be mono or stereo, each retaining existing PluginRack behavior (plugins, gain, trim, pan, mute, solo, meters, routing).

**Architecture:** Add an `inputChannels` physical-input mapping to `pluginrack::Channel` with codec defaults for old scenes and validation. Capture all hardware input channels once in a shared `InputCapture` per device; logical channels store only their mapping and pull selected physical channels. Add a minimal split/selector UI and document the behavior.

**Tech Stack:** C++20, JUCE 9 (app), nlohmann_json (core), MSVC `/W4 /WX`, no new dependencies.

**Spec:** original design doc removed from repo root; plan is self-contained.

## Global Constraints

- C++20, MSVC `/W4 /WX` clean (warnings are errors).
- No new dependencies.
- Preserve plugin/routing semantics; do not change mixer math.
- One hardware device opened once per logical source (no per-channel device opens).
- Missing physical input writes silence and reports an explicit error — never silently maps to input 0.
- Audio thread: no allocations, no device opens, no blocking locks.
- Conventional Commit messages, no AI attribution trailers.
- Never break `ctest --preset dev` (44 tests green at start) or `pluginrack_app` build.

## Review Focus

- Old scenes (no `inputChannels`) must load with Mono=[0], Stereo=[0,1] and save identically to before except the new field. (Task 1)
- Stereo indexes must be distinct and non-negative; invalid counts rejected with a message naming channel + mapping. (Task 1)
- A missing physical input must produce silence + visible error, not wrong-channel audio. (Task 2/3)
- Shared capture must be opened once even with N logical channels. (Task 3)
- Normal rendering must perform no new allocations or device opens. (Task 3)

---

### Task 1: Core Scene model — inputChannels + codec + validator

**Files:**
- Modify: `core/include/pluginrack/Scene.h` (Channel struct ~lines 20-33)
- Modify: `core/src/SceneCodec.cpp` (Channel ser/de ~lines 83-116, version handling)
- Modify: `core/src/SceneValidator.cpp` (validate ~lines 25-125)
- Test: `tests/test_scene_codec.cpp`, `tests/test_scene_validator.cpp`

**Interfaces:**
- Consumes: existing `Channel`, `ChannelLayout`.
- Produces: `Channel::inputChannels` (`std::vector<int>`); codec reads/writes `"inputChannels"` array; validator rejects invalid mappings with a useful message. No schema version bump (field is optional with defaults).

- [ ] **Step 1: Write failing tests.** Add to `test_scene_codec.cpp`: Mono mapping round-trips; Stereo mapping round-trips; old scene without `inputChannels` receives Mono=[0]/Stereo=[0,1]. Add to `test_scene_validator.cpp`: invalid mapping counts rejected; negative and duplicate indexes rejected; error message contains channel name/id and the invalid mapping.
- [ ] **Step 2: Run to verify failure.** `ctest --preset dev` — new tests fail (no field).
- [ ] **Step 3: Implement model.** Add `std::vector<int> inputChannels;` to `Channel` in `Scene.h`. In codec `from_json`, mirror the `if (j.contains("layout"))` pattern: if `inputChannels` present, read vector; else apply defaults (Mono→[0], Stereo→[1? no: [0,1]]). In `to_json`, always write `inputChannels`. No version bump.
- [ ] **Step 4: Implement validation.** In `SceneValidator::validate`, per channel: if Mono, require exactly 1 index; if Stereo, exactly 2 distinct; all >= 0. Add a new `SceneErrorCode` value (e.g. `InvalidInputMapping`) or reuse `ValueOutOfRange` with a clear message. Error order: after existing channel field checks.
- [ ] **Step 5: Run tests.** `ctest --preset dev` — all pass.
- [ ] **Step 6: Commit.** `git add core tests`; message `feat: add inputChannels mapping to scene model`.

---

### Task 2: DeviceStream multi-channel capture + pull

**Files:**
- Modify: `app/src/DeviceStream.h` (`InputCapture` lines 14-46)
- Modify: `app/src/DeviceStream.cpp` (`InputCapture::open` lines 47-99, `pull` 100-129, callback 131-159)

**Interfaces:**
- Consumes: device's actual input-channel count.
- Produces: `InputCapture::open(preferredDeviceName, engineSampleRate, numChannelsNeeded, errorOut)`; `pull(dest, frames, const std::vector<int>& physicalChannels)` copying 1 (mono) or 2 (stereo) source channels into `dest` (2 channels for stereo, 1 for mono); returns bool + sets an error/status for missing inputs. `InputCapture::inputChannelCount()`.

- [ ] **Step 1: Open with required input count.** Change `manager.initialise(2, 0, ...)` to `initialise(numInputChannels, 0, ...)` where `numInputChannels = max(2, highestIndex+1)` (at least 2 for stereo compat). Store actual `inputChannelCount_`.
- [ ] **Step 2: Allocate FIFO storage with actual count.** `storage.setSize(inputChannelCount_, capacity)`. No duplication.
- [ ] **Step 3: Copy every available input in the callback.** Remove the `else if (numInputChannels > 0) srcCh = 0;` fallback (line 141-144). For channels beyond what the device exposes, leave silence and set an `inputUnavailable_` flag + error string.
- [ ] **Step 4: Implement pull with mapping.** `pull(dest, frames, physicalChannels)`: if `physicalChannels.size()` is 1, copy that source channel into dest channel 0; if 2, copy into dest 0/1 preserving order. If a requested index >= inputChannelCount_, write silence to the affected dest channel and return false (report error). Do not resize `dest` (caller sizes it).
- [ ] **Step 5: Track unavailable channels.** Expose `getInputError()` describing missing inputs.
- [ ] **Step 6: Verify.** Build clean; manual smoke. Commit `feat: capture all hardware inputs once with mapping pull`.

---

### Task 3: AudioEngine shared capture + mapping-based render

**Files:**
- Modify: `app/src/AudioEngine.h` (Channel struct lines 25-39; remove per-channel `input`? keep as shared pointer; add mapping)
- Modify: `app/src/AudioEngine.cpp` (render lines 37-133; addChannel 159-175; makeScene 355-383; restoreScene 385-473; setChannelInputDevice 475-510)

**Interfaces:**
- Consumes: `InputCapture` from Task 2, `Channel::inputChannels` from Task 1.
- Produces: shared `std::shared_ptr<InputCapture> inputCapture_` keyed by device name; `Channel` stores only `inputChannels` + `inputDeviceName` + `inputError`. `AudioEngine::prepareInputs()` opens shared capture once based on highest index needed.

- [ ] **Step 1: Replace per-channel capture ownership.** Change `Channel::input` to not own the capture; instead store `std::vector<int> inputChannels` (mirror scene). Add `AudioEngine::std::shared_ptr<InputCapture> sharedInput_` (or map deviceName→capture).
- [ ] **Step 2: Open shared capture once.** In `restoreScene` / `setChannelInputDevice`, compute the highest physical index needed across all channels on that device; open one capture with that count; assign to all those channels. Surface errors via the existing channel/device error path.
- [ ] **Step 3: Render using mapping.** In `render`, for each channel: size buffer to (layout channels, frames) [mono→1, stereo→2, but keep 2 for compat? keep existing 2-ch buffer and fill], call `sharedInput_->pull(channel->buffer, frames, channel->inputChannels)`. Preserve inserts, gain, mute/solo, meter, routing.
- [ ] **Step 4: Fix makeScene.** Write `sourceKind`, `sourceDeviceId` (from channel.inputDeviceName or a stored device id), `layout` (actual, not hardcoded Stereo), and `inputChannels`.
- [ ] **Step 5: Fix restoreScene.** Read those fields; apply layout/channels; then open devices (shared) and set names. Keep the existing swap-under-lock pattern.
- [ ] **Step 6: No allocations on audio thread.** Ensure buffer sizes are preallocated in prepare; pull does not resize.
- [ ] **Step 7: Verify and commit.** Build clean; commit `feat: shared input capture with physical mapping in AudioEngine`.

---

### Task 4: ChannelStrip UI — mode, mapping, split action

**Files:**
- Modify: `app/src/ChannelStrip.h` / `.cpp`
- Modify: `app/src/MixerComponent.cpp` (parent for split action, rebuild)

**Interfaces:**
- Produces: channel header shows source device name, mode (Mono/Stereo), physical input selectors (1-based). `Split into mono channels` action with confirmation.

- [ ] **Step 1: Expose mode + inputs in header.** Add a mode selector (Mono/Stereo) and one or two input selectors (1-based) to the device row or a new 22 px row. Use existing JUCE controls + theme tokens.
- [ ] **Step 2: Wire to engine.** Changing mode/inputs updates the channel's layout + inputChannels via AudioEngine API and re-prepares inputs. Show unavailable inputs as an error (non-color indicator too).
- [ ] **Step 3: Split action.** Add `Split into mono channels` button/menu on a multichannel source. On confirm, create one mono channel per available physical input with names `Input 1`, etc. Preserve original channel state on the first; new channels empty. Require confirmation before creating multiple channels.
- [ ] **Step 4: Unavailable mappings visible.** If a mapping points past available inputs, show an error state, not a disabled control.
- [ ] **Step 5: Verify visually.** Build clean; screenshot at 1600x900 and 1180x700; commit `feat: split/selector UI for multichannel inputs`.

---

### Task 5: Tests

**Files:**
- Modify: `tests/test_scene_codec.cpp`, `tests/test_scene_validator.cpp`
- Create: `tests/test_input_mapping.cpp` (core or app-level) + add to `tests/CMakeLists.txt`

**Interfaces:**
- Produces: tests for scene data, capture/mapping behavior with a deterministic fake capture, regression of mono/stereo rendering.

- [ ] **Step 1: Scene data tests.** Mono/Stereo mapping round-trip; old scenes receive defaults; invalid counts rejected; negative/duplicate rejected.
- [ ] **Step 2: Capture/mapping tests.** Use a deterministic fake capture (buffer fixture) — not a real device. Mono receives only selected input; stereo receives selected L/R; two logical channels read different mappings from one shared capture; fan-out allowed; missing input → silence + error.
- [ ] **Step 3: Regression.** Existing mono/stereo rendering unchanged; gain/pan/mute/solo/plugin/meter/routing unchanged; existing scenes load.
- [ ] **Step 4: Run.** `ctest --preset dev` all pass; commit `test: input mapping and shared capture`.

---

### Task 6: Documentation and ship

**Files:**
- Modify: `ui.md` (physical input assignment, mono/stereo in headers, 1-based numbers, split action, non-color indicators)
- Possibly `PRODUCT.md` if it defines source behavior.

- [ ] **Step 1: Update ui.md** per spec section 6.
- [ ] **Step 2: Full build + tests.** `cmake --build --preset dev --target pluginrack_app` + `ctest --preset dev`.
- [ ] **Step 3: Manual test** with a 4-input interface per spec validation commands.
- [ ] **Step 4: Ship.** Copy exe; push only if all pass.
