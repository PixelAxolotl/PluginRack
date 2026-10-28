# Task 8 — Fix round 1

## Findings fixed
1. Zero-frame renders returned no buffers: the `numFrames <= 0`
   early `return {}` ran before output buffers were allocated, so a
   render of 0 frames returned an empty vector instead of one buffer
   per output, breaking the CLI for `--seconds` values that truncate
   to 0 frames.
   - `core/src/OfflineRenderer.cpp`: moved the `numFrames <= 0` guard
     below `engine.prepare()` and the `result(numOutputs)` allocation,
     mirroring the normal path's order. Zero-frame renders now return
     one empty buffer per output; `reserve()` stays after the guard so
     no sign-conversion on non-positive counts. `prepare()` still runs
     first, so `renderSine` still throws `std::runtime_error` on prepare
     failure for every input (no `noexcept` added).
   - `writeWav` already handles 0 frames (empty span writes a
     header-only file with `dataBytes == 0`); untouched.

## Verification
- Build via `vcvars64.bat` (BuildTools 18) + `cmake --build --preset dev`.
- `ctest --preset dev`: 33/33 pass.
