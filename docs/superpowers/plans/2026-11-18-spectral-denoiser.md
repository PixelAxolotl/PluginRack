# Spectral Denoiser Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a scene-persisted, low-latency spectral noise reducer to channel and output effect chains.

**Architecture:** A new `SpectralDenoiserEffect` value type plus a streaming STFT `SpectralDenoiserProcessor` in `core`, wired through `Scene`, `SceneCodec`, `SceneValidator`, `MixerEngine`, the live `AudioEngine`, and a shared editor window used by channel and output strips. The processor learns or adaptively tracks per-bin noise power, applies a linked stereo Wiener-style gain, and reports a fixed 512-sample latency.

**Tech Stack:** C++20, CMake/Ninja, Catch2, JUCE (app UI), nlohmann/json.

**Spec:** `docs/superpowers/specs/2026-11-18-spectral-denoiser-design.md`

## Global Constraints

- After every source or configuration change run `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`; the expected artifact is `PluginRack.exe` in the repository root.
- Conventional Commits only; no AI attribution trailers.
- Do not touch `docs/superpowers/specs/2026-11-18-noise-gate-design.md`.
- Audio callback: no allocation, locks, file I/O, or JSON.
- Original DSP; do not copy Bertom code or branding.
- Scene schema stays 8; new keys are additive and default when missing.
- Denoiser frame is 512 samples with 128-sample hop; fixed latency is 512 samples (10.7 ms at 48 kHz). This corrects the spec's 1024-point frame, which cannot meet the stated latency; update the spec in Task 1.

## Review Focus

- First 512 samples after instantiation: output is silence/fade-in, never garbage or an uninitialized read.
- Sample-rate change: a saved profile is remapped by frequency, not reinterpreted by bin index.
- Stereo: both channels receive one linked gain mask, so the image does not wander.
- Removal/bypass: removing a denoiser row silences it; bypass preserves the fixed delay.
- Silent or non-finite input: output stays finite and no NaN enters the chain.

---

### Task 1: Core effect and processor

**Files:**
- Create: `core/include/pluginrack/SpectralDenoiserEffect.h`
- Create: `core/src/SpectralDenoiserEffect.cpp`
- Modify: `core/CMakeLists.txt` (add `src/SpectralDenoiserEffect.cpp`)
- Modify: `docs/superpowers/specs/2026-11-18-spectral-denoiser-design.md` (frame/hop/latency values)
- Test: `tests/test_spectral_denoiser.cpp`

**Interfaces:**
- Produces: `pluginrack::SpectralDenoiserEffect`, `makeDefaultSpectralDenoiserEffect(std::uint32_t)`, `SpectralDenoiserProcessor::prepare(double)`, `setEffect(const SpectralDenoiserEffect&)`, `process(float*, float*, int) noexcept`, `reset() noexcept`, `startLearning() noexcept`, `isLearning() const noexcept`, `learnProgress() const noexcept`, `takeLearnedProfile(std::vector<float>&, double&) noexcept`, `static constexpr int latencySamples() noexcept`, `currentReductionDb() const noexcept`.
- Produces: constants `kSpectralDenoiserFftSize = 512`, `kSpectralDenoiserHopSize = 128`, `kSpectralDenoiserBinCount = 257`, `kSpectralDenoiserLatencySamples = 512`, `kSpectralDenoiserMinReductionDb = 0`, `kSpectralDenoiserMaxReductionDb = 40`, `kSpectralDenoiserDefaultReductionDb = 12`.

- [ ] **Step 1: Write failing tests** for zero-reduction delay (`output[i] == input[i-512]` after 1024 samples), bypass delay, stationary-noise reduction, learn profile shape/rate, and finite output on NaN input.
- [ ] **Step 2: Run the tests** with the VS environment and confirm they fail to compile or fail.
- [ ] **Step 3: Implement the effect header and processor.** Use Hann analysis/synthesis windows, 4x overlap normalized overlap-add, a local radix-2 FFT, adaptive asymmetric noise tracking, per-bin attack/release smoothing, a 3-bin smoothing pass, and one linked stereo gain.
- [ ] **Step 4: Re-run the focused test and make it pass.**
- [ ] **Step 5: Commit** `feat(core): add spectral denoiser processor`.

### Task 2: Scene model, codec, validator

**Files:**
- Modify: `core/include/pluginrack/Scene.h` (`ChainItemType::SpectralDenoiser`, `Strip::spectralDenoiserEffects`)
- Modify: `core/src/SceneCodec.cpp` (enum strings, effect JSON, strip key)
- Modify: `core/src/SceneValidator.cpp` (cap 3, id/range/profile checks, chain case)
- Test: `tests/test_spectral_denoiser.cpp`

**Interfaces:**
- Consumes: `SpectralDenoiserEffect` from Task 1.
- Produces: JSON keys `spectralDenoiserEffects`, `noiseProfile`, `profileSampleRate`, `reductionDb`, `mode`; chain string `"SpectralDenoiser"`.

- [ ] **Step 1: Add failing round-trip and invalid-profile tests.**
- [ ] **Step 2: Implement additive codec/validator support.** Reject wrong bin count, non-finite/negative bins, non-positive profile rate, reduction outside 0..40, and unknown mode strings.
- [ ] **Step 3: Run the focused tests and the scene tests.**
- [ ] **Step 4: Commit** `feat(core): persist spectral denoiser settings`.

### Task 3: Offline mixer

**Files:**
- Modify: `core/include/pluginrack/MixerEngine.h`
- Modify: `core/src/MixerEngine.cpp`
- Test: `tests/test_spectral_denoiser.cpp`

**Interfaces:**
- Adds `StripDsp::spectralDenoisers`, `ChainItemType::SpectralDenoiser` resolution, and the unresolved append after FeedbackSuppressor.

- [ ] **Step 1: Add a failing mixer test** that a scene channel with a denoiser reduces steady noise and preserves chain order.
- [ ] **Step 2: Implement build/prepare/process wiring.**
- [ ] **Step 3: Run the focused and mixer tests.**
- [ ] **Step 4: Commit** `feat(core): run spectral denoiser in the mixer`.

### Task 4: Live engine and learn API

**Files:**
- Modify: `app/src/AudioEngine.h`
- Modify: `app/src/AudioEngine.cpp`

**Interfaces:**
- Produces `setChannelSpectralDenoisers` / `setOutputSpectralDenoisers`, `startChannelDenoiserLearn` / `startOutputDenoiserLearn`, `getChannelDenoiserLearnProgress` / output equivalent, and `takeChannelDenoiserLearnedProfile(std::uint32_t, std::uint32_t, std::vector<float>&, double&)` / output equivalent.
- Adds processor vectors, render-chain cases, prepare/reset loops, `processNewMissing` calls, and removal bypass handling for both strips.

- [ ] **Step 1: Implement the engine plumbing** without adding a UI.
- [ ] **Step 2: Build the app target** and fix compile errors.
- [ ] **Step 3: Commit** `feat(app): run spectral denoiser in the live engine`.

### Task 5: Editor window and channel strip

**Files:**
- Create: `app/src/SpectralDenoiserWindow.h`
- Create: `app/src/SpectralDenoiserWindow.cpp`
- Modify: `app/CMakeLists.txt`
- Modify: `app/src/ChannelStrip.h`
- Modify: `app/src/ChannelStrip.cpp`

**Interfaces:**
- `SpectralDenoiserWindow(pluginrack::SpectralDenoiserEffect effect, AudioEngine& engine, bool output, std::uint32_t stripId, std::function<void(const pluginrack::SpectralDenoiserEffect&, bool)> apply)`.
- Public `setBypassed(bool)`, `std::function<void()> onClose`.

- [ ] **Step 1: Build the window** with mode/reduction controls, Learn/Clear/Bypass buttons, a status label, a 100 ms timer that polls learn progress and applies a finished profile.
- [ ] **Step 2: Wire channel strip** open, chain display, activate/erase, and chain-creation paths.
- [ ] **Step 3: Build and commit** `feat(app): add spectral denoiser editor`.

### Task 6: Output strip and final verification

**Files:**
- Modify: `app/src/MixerComponent.h`
- Modify: `app/src/MixerComponent.cpp`

- [ ] **Step 1: Mirror the channel window and chain handling for `OutputStrip`.**
- [ ] **Step 2: Run the full suite** via `cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" >nul 2>&1 && ctest --preset dev"`.
- [ ] **Step 3: Run** `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`; confirm the root `PluginRack.exe` timestamp is fresh.
- [ ] **Step 4: Commit** `feat(app): add spectral denoiser to output strips`.
