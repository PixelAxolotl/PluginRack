# Settings Page Usability Implementation Plan

> **For agentic workers:** Work task by task. Steps use checkbox (`- [ ]`) syntax for tracking. Read `AGENTS.md`, `PRODUCT.md` and this whole plan before the first edit.

**Goal:** Make the Settings page easy to use for a streamer, not just an audio engineer. Put everyday controls first, hide expert switches behind an "Advanced" disclosure, use plain wording with visible one-line help, make the Apply/immediate distinction obvious, and give a one-line verdict on audio health.

**Non-goals:** No engine, scene-schema, or audio-thread changes. `AudioEngine`, `DeviceSettings`, `SceneCodec` and every `staged_.*` field keep their current meaning. No new settings and no removed settings. The other pages and the toolbar are out of scope (toolbar tooltips already exist, and the unfinished Snapshot button is already hidden in `MainComponent.cpp`).

**Tech stack:** C++20, MSVC `/W4 /WX`, CMake ≥ 3.28 + Ninja, JUCE 9.0.4, Catch2 3.7.1. Windows x64 only.

**Files in scope:**
- `app/src/SettingsView.h`, `app/src/SettingsView.cpp`: almost all of the work.
- `app/src/Theme.h`: only if a shared helper is needed (for example a help-text font or a disclosure arrow). Reuse `theme::ui::*` first.
- `tests/test_underrun_paths.cpp`: only where a pinned string must change (see Global Constraints).

## Global Constraints

- **Commits:** Conventional Commits (`feat(settings): …`, `refactor(settings): …`). **Never** add `Co-authored-by`, `Generated-by` or any other AI-attribution trailer. Before committing in a Hoplite workspace, run `git config hoplite.commitCoAuthor false`. Do not push or open a PR unless the user asks.
- **Build artifact:** after each task that touches app sources, run `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1` and confirm a fresh `PluginRack.exe` lands in the repo root. If you cannot build (no Windows/JUCE/VST3 SDK), say so explicitly; do not claim completion.
- **Tests:** `ctest --test-dir build/dev --output-on-failure` must stay green after each task.
- **Source-scan tests pin strings in Settings.** `tests/test_underrun_paths.cpp` greps `SettingsView.h/.cpp` for these. Keep each one, or change the test in the same commit with a one-line reason in the commit body:
  - User-facing: `"Pending changes - press Apply"`, `"Reload All Audio Devices"`, `"Auto-reload checked devices on error"`, `"Cap WASAPI channel probe"`, `"No ASIO driver installed"`, and the tooltip phrases `"about a third"` and `"Windows default"`.
  - Code: member names (`slicedRenderToggle`, `scrubToggle`, `healthReloadToggle`, `ladderRecoveryToggle`, `healthSensitivityCombo`, `probeCapToggle`, `probeCapChannels`, `reloadDeviceToggles`, `autoReloadToggle`, `forceAsioButton`), and the calls `probeCapToggle.setEnabled(!busy)`, `probeCapChannels.setEnabled(!busy`, `forceAsioButton.setEnabled`, `budgetView.setReport(report)`, `LatencyBudgetView::preferredHeight()`, `audioEngine.getRenderTelemetry()`, `render.renderWorstOverBlockPercent`, `applyDeviceSettings`, `showDeviceBuffer`, `LowLatencyInput`, `MinimalClockFifo`, `kMaxAutoReloadRounds`, `kAutoReloadCooldownMs`, the `wasapiProbeCap* != applied.*` comparisons, and the `audioEngine.*LatencyProfile*` calls.
  - **Rule:** do not rename members. Rename only the visible text, and keep the old technical phrase in the tooltip where a test pins it.
- **Keep existing behaviour:** combos keep `setScrollWheelEnabled(false)`. Apply/Revert stay disabled while `audioEngine.isOpeningDevices()`. Immediate-apply controls (thumbnails, previews, peak hold, tray, auto-reload, reload buttons) stay immediate.
- **Layout:** everything is laid out by hand in `SettingsView::layoutBody(int width)` using `startSection` / `endSection` and panel rectangles painted by `Body`. Keep that pattern. Do not introduce FlexBox/Grid for one page. Any height change must flow through `layoutBodyToFit()` so the viewport's scroll height stays right.
- **Width:** the window can be narrow (≈ 900 px). Rows that are currently fixed-width (`removeFromLeft(320)`, `430`, `380`) must not overflow; clamp with `juce::jmin(..., inner.getWidth())`.

## Current state (read before Task 1)

`layoutBody` draws six sections in this order: **AUDIO STATUS**, **BUFFER SIZE**, **ROUND-TRIP LATENCY** (beta), **LATENCY TUNING (OPT-IN)** (beta; five tuning toggles, the WASAPI probe cap, four hardening toggles, health sensitivity, "Find low-latency setting", "Force ASIO"), **APP** (thumbnails, previews, peak hold, tray), **AUDIO DEVICES** (reload all, per-device checkboxes, auto-reload). The page header holds the title, a status word, `pendingLabel`, Apply and Revert.

Problems to fix:
1. Everyday controls (devices, app options) sit at the bottom, under expert switches.
2. Toggle labels are engineer jargon. Explanations live only in hover tooltips.
3. Some controls need Apply and some act at once, and nothing says which.
4. The status panel shows raw numbers and counters, not a plain verdict.

---

### Task 1: Reorder sections and rename captions

**Files:** `SettingsView.h`, `SettingsView.cpp`

- [ ] In `layoutBody`, reorder the section blocks to: **Status** → **Audio devices** → **Buffer size** → **General** (was APP) → **Round-trip latency** → **Advanced** (was LATENCY TUNING). Pure move of the existing `{ ... }` blocks; no geometry changes yet.
- [ ] Caption text (keep the all-caps section style): `STATUS`, `AUDIO DEVICES`, `BUFFER SIZE`, `GENERAL`, `ROUND-TRIP LATENCY`, `ADVANCED`.
- [ ] Update the comment at the top of `SettingsView.h` that says "six sections".
- [ ] Build, run the tests, launch the app, scroll the page top to bottom, and confirm no controls overlap.

Commit: `refactor(settings): put everyday sections before advanced tuning`

### Task 2: Collapsible "Advanced" section

**Files:** `SettingsView.h`, `SettingsView.cpp`

- [ ] Add `bool advancedOpen_ = false;` and a clickable caption: either a `juce::TextButton` styled flat, or make the caption label a small component with `mouseUp`. Show `▸ ADVANCED` when closed and `▾ ADVANCED` when open, with the BETA badge kept.
- [ ] When closed, `layoutBody` places the caption only. Every Advanced child gets `setVisible(false)` (or empty bounds) and no panel rectangle. When open, lay out as today. Toggling calls `layoutBodyToFit()`.
- [ ] Split the open Advanced panel into two sub-groups with small sub-captions:
  - **Lower latency:** "Find low-latency setting" button + status first (recommended path), then the five tuning toggles, the probe cap toggle + channel field, and "Force ASIO" + status.
  - **Stability & recovery:** sliced render, scrub, health reload, ladder recovery, health sensitivity.
- [ ] **Auto-open rule:** if any advanced option differs from its default (`staged_.latencyTuningOptIn`, `wasapiProbeCapEnabled`, `slicedRender`, `nonFiniteScrub`, `healthReload`, `ladderRecovery`, non-default health thresholds), or a latency profile run is active, open the section on construction. Users must never have a hidden setting switched on without seeing it. Do not persist `advancedOpen_`.
- [ ] Optionally collapse **Round-trip latency** the same way (closed unless a measurement exists or is running). Recommended, but keep it in a separate commit.

Commit: `feat(settings): collapse advanced latency and stability options`

### Task 3: Plain-language labels with visible help lines

**Files:** `SettingsView.h`, `SettingsView.cpp` (and the test file only if a pinned string changes)

- [ ] For each toggle, change the **visible** label and add a dim one-line help `WrappedLabel` under it (use `theme::ui::captionFont()` and `theme::dim()`). Keep the existing technical tooltip unchanged so the detail and the pinned phrases survive. Suggested wording:

| Member | New label | Help line |
|---|---|---|
| `lowLatencyInputToggle` | Faster input | Saves about 10 ms. Adds a little back by itself if the driver stutters. |
| `exclusiveToggle` | Exclusive output (WASAPI) | Takes over the main output device for lower delay. Other apps can't use it. |
| `lowLatencyOutputsToggle` | Faster extra outputs | Saves about 20 ms on outputs other than the main one. |
| `minimalClockFifoToggle` | Tighter main output | Saves about 10 ms. A late block plays as a short silence. |
| `fullDuplexToggle` | Same-device input sync | When input and output are the same device, run them together. |
| `probeCapToggle` | **keep** "Cap WASAPI channel probe" (pinned) | Faster startup for devices with many channels. |
| `slicedRenderToggle` | Limit damage from slow plugins | A stalling plugin drops a sliver of audio, not a whole block. |
| `scrubToggle` | Protect against broken plugin output | Silences invalid samples so one bad plugin can't blast every output. |
| `healthReloadToggle` | Restart unhealthy streams | Reopens a stream that keeps glitching. |
| `ladderRecoveryToggle` | Recover quality after problems | After 10 clean minutes, step back toward lower latency. |
| `autoReloadToggle` | **keep** "Auto-reload checked devices on error" (pinned) | Tries three times, five seconds apart. |
| `closeToTrayToggle` | Keep running in the tray when closed | *(none needed)* |

- [ ] Add a short help line under **Buffer size**: "Smaller = less delay, more risk of crackles. Start at 256 or 480 and use *Find low-latency setting* to go lower."
- [ ] Add a help line under **Audio devices**: "Tick the devices to watch. Ticked devices restart by themselves if they fail."
- [ ] Layout: each toggle row becomes toggle (24 px) + help line (`WrappedLabel::heightFor(...)`) + gap. Compute heights in `layoutBody` so wrapping at narrow widths does not clip.

Commit: `feat(settings): plain-language labels and inline help`

### Task 4: Make "needs Apply" vs "applies immediately" obvious

**Files:** `SettingsView.cpp`, `tests/test_underrun_paths.cpp` only if the pending text changes

- [ ] Add a small dim tag at the right end of each section caption row. Use "Needs Apply" for Buffer size and Advanced, and "Applies immediately" for Audio devices and General. Status and Round-trip get no tag.
- [ ] When `dirty_` is true, draw the Apply button in a highlight colour (`theme::cyan()`; `Theme.h` has no `accent()`), and keep the text `"Pending changes - press Apply"` (pinned) in amber instead of dim.
- [ ] When the user changes a staged control, give that section's panel a thin amber left edge until Apply/Revert. Use a per-section dirty check that compares the relevant `staged_` fields with `audioEngine.getDeviceSettings()`; reuse the field list in `updatePendingState()`. Pass a colour per panel through `Body::setPanels` (for example `std::vector<std::pair<Rectangle, bool>>`) instead of adding new components.

Commit: `feat(settings): show which settings wait for Apply`

### Task 5: Plain verdict in the Status panel

**Files:** `SettingsView.cpp`

- [ ] Above the three stats, add one bold line coloured like the existing `issuesColour` logic in `refresh()`:
  - opening devices → "Starting audio…" (amber)
  - `deviceIssues` non-empty → "A device has a problem. See below." (red)
  - `countedIssues` non-empty → "Audio is glitching. Try a larger buffer." (amber)
  - otherwise → "Audio is running normally" (green)
- [ ] Keep the detailed `issuesText` underneath as is, but drop the redundant "No audio errors." body text when everything is fine (the verdict already says it). Do not change the header `setStatus` calls.
- [ ] Rename stat captions for clarity: `CLOCK` → `MAIN DEVICE`, `LATENCY (EST.)` → `DELAY (EST.)`. Leave the `BUFFER` caption logic alone.
- [ ] Re-check `relayout` handling: the verdict line height must be part of the panel height in `layoutBody`.

Commit: `feat(settings): plain audio health verdict`

---

## Review focus

1. **Hidden but active settings.** The Advanced section must auto-open whenever any advanced option is non-default (Task 2). Check with a scene that has `nonFiniteScrub` on.
2. **Apply semantics unchanged.** Every staged control still only reaches the engine through `applyDeviceSettings(staged_)`, and the immediate ones still act at once. Diff `onClick`/`onChange` lambdas; they should not change.
3. **Busy state.** While devices open, Apply/Revert, the probe cap controls and "Force ASIO" stay disabled exactly as before.
4. **Layout at narrow width and with long device names.** No overlap at ~900 px wide, and wrapped help lines are not clipped. Scroll height matches content after toggling Advanced open and closed.
5. **Tests.** Every pinned string is still present, or its test was updated in the same commit with a reason.

## Manual acceptance (Windows)

- [ ] Fresh start: the page opens with Status, Devices, Buffer and General visible, and Advanced is closed.
- [ ] Change the buffer: Apply turns highlighted, the Buffer panel shows an amber edge, and Revert clears both.
- [ ] Open Advanced, toggle "Protect against broken plugin output", Apply, then restart and reload the scene: Advanced opens by itself and the toggle is on.
- [ ] Unplug a watched device: the verdict turns red, and auto-reload behaves as before.
- [ ] Take screenshots before and after, and save them under `screenshots/` (for example `settings-usability-before.png` / `-after.png`).
