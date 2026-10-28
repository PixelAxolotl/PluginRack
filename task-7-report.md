# Task 7 — Fix round 1

## Findings fixed
1. Output metering skipped when `output.channels == nullptr`: the
   null-channel early `continue` ran before the accumulator peak was
   computed and observed, so an output with no writable pointers never
   updated its meter.
   - `core/src/MixerEngine.cpp`: compute the accumulator peak and call
     `meters_.observeOutput()` before the `output.channels == nullptr`
     early return, mirroring the channel-peak observe path above it.
     Peak is still measured from the summed accumulator, so disabled /
     unwritable outputs meter post-send signal; only the sample writes
     stay guarded by the null/disabled checks.
2. `Meters.cpp` `std::clamp(NaN, 0, 8)` stays NaN: NaN comparisons are
   false, so the clamp passed NaN through and poisoned the stored peak.
   - `core/src/Meters.cpp`: `clampPeak()` returns `0.0f` for non-finite
     input (`!std::isfinite(peak)`) before clamping to `[0, 8]`
     (+18 dB headroom over the +12 dB fader). Same `noexcept` /
     allocation-free path; `<cmath>` was already included.

## Verification
- Build via `vcvars64.bat` (BuildTools 18) + `cmake --build --preset dev`.
- `ctest --preset dev`: 30/30 pass.
- `MixerEngine::render` untouched in signature, still `noexcept` /
  allocation-free (output ids are pre-registered in `prepare()`, so the
  moved `observeOutput()` never allocates).
