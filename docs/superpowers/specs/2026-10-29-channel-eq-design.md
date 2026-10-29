# Channel EQ Design (PluginRack) — Approach A

Date: 2026-10-29
Status: approved — user chose A (native JUCE parametric + FFT popup), inputs + outputs, curve + analyzer, saved in scene.

## Goal

Every input channel strip and every output/master strip gets a built-in FabFilter Pro-Q 3-style parametric EQ, opened from an EQ button placed where Voicemeeter Potato has its EQ. Curve editor popup with draggable nodes, per-band controls, live spectrum analyzer, persisted in scene.

Supersedes the "6-band EQ on outputs as third build" placeholder in `2026-10-29-output-fx-design.md:3,15,17`.

## Decisions (locked by user)

- Full Pro-Q clone scope, not 3-knob fallback.
- Coverage: inputs + outputs (A1–A5, B1–B3).
- v1 includes curve + live analyzer (no phased split).
- Persisted in scene (schema v2 → v3, auto-migrate).
- Popup window, not inline curve (strips are 148 px inputs / 120 px outputs — `MixerComponent.cpp:292-302` — inline cannot fit).

## Architecture

- New `EqEngine` in `core/` — JUCE-free biquads (DF-II transposed) so `core/MixerEngine` CLI/tests run bit-identical without JUCE.
- Thin JUCE wrapper in `app/`; add `juce_dsp` link in `app/CMakeLists.txt:63-77` (currently not linked; zero `juce::dsp`/`FFT` hits in repo).
- Tap point pre-fader:
  - app: `AudioEngine::render` at `AudioEngine.cpp:103-105` (after input pull `83-88`, with/before inserts `100-103`, before mute/solo/gain/pan `105-109`).
  - core mirror: `MixerEngine.cpp:225-230` (before gain+pan `191-257`), routes/outputs mirror `259-294`.
- State:
  - `AudioEngine::Channel` (`AudioEngine.h:28-47`) — add `eqBands[6]` as `juce::Atomic<float>` + `needsEqUpdate`; render thread guarded by `stateLock` (`AudioEngine.cpp:68`).
  - `pluginrack::Channel` (`Scene.h:20-34`) — add `eqBands`; `SceneCodec.cpp:91-95,119-124` read/write; schema accept 2+3, save writes v3 (`SceneCodec.cpp:189-196` pattern).
- UI host: `EQWindow : juce::DocumentWindow` mirroring `PluginWindow` (`PluginHost.cpp:4-8`); alternative `RackView` centre editor (`RackView.h:77-85`) rejected — popup matches plugin-editor precedent and works for outputs.

## Components

- DSP bands (fixed 6 to start): HP 10 Hz–500 Hz, Low-shelf, Bell x3, High-shelf to 20 kHz. Per band: freq/gain/Q, bypass, solo. Coeff calc off audio thread, preallocated state (respects `AudioEngine.h:192-195` never-allocates rule).
- Strip button: `EQ` toggle in gap row `ChannelStrip.cpp:238-241` (inputs) + output strips; header row `204-210` fallback. Amber when EQ active, cyan focus ring per DESIGN.md. Routing grid `250-261` untouched.
- EQWindow popup: log-freq X 20 Hz–20 kHz, ±15 dB Y, Pro-Q grid; draggable nodes with band colors, yellow combined curve; bottom knob row Freq/Gain/Q + Auto + Stereo tag (visual only v1); analyzer Pre+Post/Off switch.
- Analyzer: `juce::dsp::FFT` 2048 Hann, 60 fps decay, grey fill behind curve; one FFT per open window only (not per strip). FFT on UI timer, never audio thread.

## Data flow

Drag/knob → `EqParams` → atomics + `needsUpdate` → message-thread coeff rebuild → lock-guarded swap. Audio thread reads atomics + preallocated biquad states only. Analyzer taps post-EQ buffer copy. Scene save writes bands; load validates via `SceneValidator`.

## Error handling

Freq/Q clamped, NaN → bypass band. Fixed 6 bands (no dynamic add). FFT overload → analyzer drops to 30 fps, never drops audio. v2 scene → flat EQ defaults, saves as v3. Missing `juce_dsp` → CMake configure error with message.

## Product constraint exception

`PRODUCT.md:19` / `DESIGN.md:24` said "VST rack replaces IntelliPan and Special FX". This build grants an explicit exception for built-in EQ only; no other built-in FX (comp/gate/denoise/reverb) are in scope.

## Acceptance

- EQ button on every input + output strip; click opens curve popup; drag node + knobs change sound.
- Bypass passes audio unchanged; solo isolates band.
- Analyzer shows Pre/Post spectrum; Off hides it.
- v2 scene loads flat, saves v3; v3 round-trips.
- `/W4 /WX` clean, all core tests green, screenshots 1600×900 + 1180×700 (EQ button + open window), smoke run alive, root `PluginRack.exe` refreshed.

## Out of scope

- Per-band M/S, dynamic EQ, linear-phase mode; output drag-reorder; presets/copy-paste (later).
