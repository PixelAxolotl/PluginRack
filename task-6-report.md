# Task 6 — Fix round 1

## Findings fixed
1. Duplicate warnings for one missing device: each channel sharing the same
   unavailable input emitted its own warning; same for outputs sharing a
   device id. Contract is one warning per missing device.
   - `core/src/SceneAvailability.cpp`: deduplicate warnings by (kind, deviceId)
     with a `seenInputs` / `seenOutputs` `unordered_set` (matches the
     `unordered_set` duplicate-detection convention in `SceneValidator`).
     Every affected channel/output is still recorded in
     `stoppedChannels`/`disabledOutputs`; only the warning string is emitted
     once, in first-seen scene order.
2. `stoppedChannels`/`disabledOutputs` documented as scene-order indices.
   - `core/include/pluginrack/SceneAvailability.h`: comments state they are
     0-based indices into `scene.channels` / `scene.outputs`, not channel/output
     ids; warnings stay `"<kind> '<deviceId>' unavailable"`, one per device.

## Verification
- Build via `vcvars64.bat` (BuildTools 18) + `cmake --build --preset dev`.
- `ctest --preset dev`: 25/25 pass.
- `MixerEngine::render` untouched, still `noexcept` / allocation-free.
