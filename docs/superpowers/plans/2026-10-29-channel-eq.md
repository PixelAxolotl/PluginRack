# Channel EQ Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Built-in 6-band parametric EQ with Pro-Q-style curve popup + live analyzer on every input and output strip.

**Architecture:** JUCE-free `EqEngine` in `core/` (bit-identical DSP) + thin `AudioEngine` wrapper in `app/`; `EQWindow : DocumentWindow` popup from strip EQ button; schema v2 → v3.

**Tech Stack:** C++20, JUCE 9 (gui + audio + `juce_dsp` IIR/FFT), CMake, Catch2 (core tests).

**Spec:** `docs/superpowers/specs/2026-10-29-channel-eq-design.md`

## Global Constraints

- Windows 10/11 x64, JUCE native app — no web tech.
- Build `/W4 /WX` clean.
- Audio thread never allocates (`app/src/AudioEngine.h:192-195`); coeff rebuild off-thread, swap under `stateLock`.
- Two engines stay in parity: `app/src/AudioEngine.cpp` + `core/src/MixerEngine.cpp`.
- Scene v2 loads flat, saves v3; v3 round-trips.
- Strips: inputs 148 px, outputs 120 px — curve lives in popup only.
- One FFT per open window only; analyzer never on audio thread.

## Review Focus

- NaN/Inf/subnormal freq/gain/Q from dragged node → expect clamp + band bypass, never silence/noise burst.
- Q=20 +12 dB bell at Nyquist-adjacent freq at 44.1k vs 96k → expect stable, no ringing blowup.
- Sample-rate / block-size change mid-session → expect coeff rebuild, no clicks beyond 1 block.
- v1 scene with no EQ keys + v2 scene with flat placeholder → expect loads flat, saves v3 with 6 bands.
- 13 strips, user opens EQ on two strips rapidly / closes via X vs button → expect single window per strip, no dangling pointer, no leak.

---

### Task 1: Core EQ DSP (`EqEngine`)

**Files:**
- Create: `core/include/pluginrack/EqEngine.h`
- Create: `core/src/EqEngine.cpp`
- Test: `tests/test_eq_engine.cpp`
- Modify: `tests/CMakeLists.txt:4` (add test), `core/CMakeLists.txt` (add src if globbed — check first)

**Interfaces:**
- Consumes: nothing (standalone).
- Produces:
  - `enum class pluginrack::EqType { HighPass, LowShelf, Peak, HighShelf };`
  - `struct pluginrack::EqBand { EqType type; float freqHz; float gainDb; float q; bool enabled; bool solo; bool operator==(const EqBand&) const = default; };`
  - `static std::vector<pluginrack::EqBand> pluginrack::defaultSixBands();` // HP 80Hz, LShelf 200Hz, Peak 500/1k/3k, HShelf 8k, all 0 dB, Q 0.707 (HP Q unused)
  - `class pluginrack::EqEngine { public: void prepare(double sampleRate, int maxBlock); void setBands(const std::vector<EqBand>&); void process(float* L, float* R, int n) noexcept; void reset() noexcept; };` // solo: if any band solo==true, only soloed bands process

- [ ] **Step 1: Write failing test** in `tests/test_eq_engine.cpp`:
```cpp
TEST_CASE("eq flat passes audio") { EqEngine e; e.prepare(44100, 512); e.setBands(defaultSixBands()); float L[8]={1,0,0,0,0,0,0,0}, R[8]={1,0,0,0,0,0,0,0}; e.process(L,R,8); REQUIRE(L[0]==Catch::Approx(1.0f).epsilon(0.001)); }
TEST_CASE("eq bypassed band is bit-identical") { /* enabled=false band vs no EQ */ }
TEST_CASE("eq HP removes DC") { /* DC in, HP 100Hz enabled → out ≈ 0 after settle */ }
TEST_CASE("eq NaN params never NaN out") { /* freq=NaN, Q=Inf → process pink noise, REQUIRE no isnan, output finite */ }
TEST_CASE("eq solo isolates band") { /* two bells, solo one → only soloed band audible (compare vs solo-off) */ }
```
- [ ] **Step 2: Run to verify it fails** Run: `cmake -B build -DPLUGINRACK_BUILD_APP=OFF; cmake --build build --target pluginrack_tests; ctest --test-dir build -R eq --output-on-failure` Expected: FAIL (target/file missing).
- [ ] **Step 3: Implement `EqEngine` in `core/...`** — DF-II transposed biquads, RBJ cookbook (Peak/LowShelf/HighShelf/HP), one state per band per channel (12 states), coeff calc in `setBands`/`prepare` only, `process` is multiply-add loop, denormal guard (`+1e-18f`), NaN guard → skip band.
- [ ] **Step 4: Run tests to verify pass** Run same ctest `-R eq`. Expected: PASS.
- [ ] **Step 5: Commit** `git add core/include/pluginrack/EqEngine.h core/src/EqEngine.cpp tests/test_eq_engine.cpp tests/CMakeLists.txt; git commit -m "feat(eq): core EqEngine 6-band biquads"`

### Task 2: Core engine integration + scene v3

**Files:**
- Modify: `core/include/pluginrack/Scene.h:20-34`, `core/include/pluginrack/MixerEngine.h:36-42`, `core/src/MixerEngine.cpp:63-75,225-242`, `core/src/SceneCodec.cpp:7,83-97,99-126,174,189-194`
- Test: `tests/test_mixer_engine.cpp` (append), `tests/test_scene_codec.cpp` (or create if missing — check)

**Interfaces:**
- Consumes: `EqBand`, `EqEngine` from Task 1.
- Produces:
  - `Scene::Channel.eqBands / eqEnabled` + same on `Scene::Output` (mirror struct used for outputs — confirm name in file).
  - `MixerEngine::setChannelEq(int ch, vector<EqBand>)`, `setOutputEq(int out, ...)` (exact names locked here).

- [ ] **Step 1: Write failing tests** — `v2 scene loads flat EQ` (parse v2 JSON, REQUIRE eqBands.size()==6, all gain 0), `v3 round-trip preserves bands`, `mixer +6dB peak at 1k boosts 1k sine ≈2x` (copy `REQUIRE(out.left[i]==Catch::Approx(...))` style from `test_mixer_engine.cpp:100-101`).
- [ ] **Step 2: Run to verify fail** Run: `ctest --test-dir build -R "scene|mixer" --output-on-failure` Expected: FAIL (no eq fields).
- [ ] **Step 3: Implement Scene + MixerEngine** — add `eqEnabled=false` + `eqBands=defaultSixBands()` to Channel/Output; codec: bump `kSchemaVersion=3`, accept `1..3`, read with `j.contains` defaults; `MixerEngine::prepare` preallocates `vector<EqEngine> eq_`; render runs `eq_.process` at `MixerEngine.cpp:225-229` before gain/pan (channels) and at `:283-294` before output gain (outputs).
- [ ] **Step 4: Run tests pass** Same ctest. Expected: PASS (existing exact-sample tests still green — flat EQ is bit-identical).
- [ ] **Step 5: Commit** `git commit -m "feat(eq): core engine tap + scene v3"`

### Task 3: App engine integration

**Files:**
- Modify: `app/CMakeLists.txt:63-77` (add `juce::juce_dsp`), `app/src/AudioEngine.h:28-47,133-137`, `app/src/AudioEngine.cpp:100-109,142-145,656,707`

**Interfaces:**
- Consumes: `EqEngine`, `setChannelEq` pattern from Task 2.
- Produces:
  - `void AudioEngine::setChannelEq(uint32_t id, const std::vector<pluginrack::EqBand>&);`
  - `void AudioEngine::setOutputEq(int outIdx, const std::vector<pluginrack::EqBand>&);`
  - `void AudioEngine::setChannelEqEnabled/Bypassed(...)` (name locked here — pick `setChannelEqBypassed(id,bool)`).

- [ ] **Step 1: Write manual check** — no unit test (JUCE engine needs device); verification is build + smoke in Step 4.
- [ ] **Step 2: Implement link + state + tap** — add `juce::juce_dsp` link; `Channel`/`Output` gain `EqEngine eq; std::vector<EqBand> eqBands; Atomic<bool> eqBypassed; Atomic<bool> eqDirty;`; tap channel EQ at `AudioEngine.cpp:100` (after input pull, before inserts) via existing `block` view; output EQ at `:142` before output inserts; coeff rebuild when `eqDirty` on message thread (never in render); `makeScene:656`/`restoreScene:707` copy eqBands (trimDb stays session-only — do not persist trim).
- [ ] **Step 3: Build `/W4 /WX` clean** Run: `cmake -B build; cmake --build build --config Release` Expected: zero warnings.
- [ ] **Step 4: Smoke** — run `build/.../PluginRack.exe` (or root exe), add channel, call setters via temp button (or wait Task 4), confirm no xrun. Expected: alive.
- [ ] **Step 5: Commit** `git commit -m "feat(eq): app engine tap + juce_dsp link"`

### Task 4: Strip EQ button + EQWindow shell

**Files:**
- Modify: `app/src/ChannelStrip.h:45-76`, `app/src/ChannelStrip.cpp:238,265,275-277`, `app/src/MixerComponent.h:27-62`, `app/src/MixerComponent.cpp:292,302`
- Create: `app/src/EQWindow.h`, `app/src/EQWindow.cpp`, `app/src/EQCurveComponent.h/.cpp` (shell in this task, full drawing in Task 5)

**Interfaces:**
- Consumes: `setChannelEqBypassed` from Task 3.
- Produces:
  - `void ChannelStrip::openEqWindow(); void ChannelStrip::closeEqWindow();`
  - `class EQWindow : public juce::DocumentWindow` (mirrors `PluginHost.cpp:4-49` — SafePointer + callAsync close).

- [ ] **Step 1: Build shell empty window** — `eqButton` ("EQ") in bottom 30 px row (`ChannelStrip.cpp:265`) → three thirds with mute/solo; output strips same in `OutputStrip`; click creates `EQWindow 520x360` with placeholder label, single instance, X + re-click closes.
- [ ] **Step 2: Verify build + screenshots** Run: build + `ctest` core still green; launch, screenshot 1600×900 showing EQ buttons. Expected: buttons visible, window opens/closes, no crash on rapid open/close.
- [ ] **Step 3: Commit** `git commit -m "feat(eq): strip EQ button + EQWindow shell"`

### Task 5: Curve editor + knob row

**Files:**
- Modify: `app/src/EQCurveComponent.*`, `app/src/EQWindow.*`

**Interfaces:**
- Consumes: `EqBand`, `setChannelEq` from Task 3.
- Produces: `class EQCurveComponent : public juce::Component` with `setBands/getBands`, node drag callback `onBandsChanged`.

- [ ] **Step 1: Implement curve** — log-freq X 20–20k, ±15 dB Y, Pro-Q grid lines; 6 draggable nodes (band colors) + yellow combined magnitude response (compute via biquad magnitude, not FFT); bottom row Freq/Gain/Q rotary knobs for selected band + bypass/solo per band + Auto button + Stereo tag (visual only v1, no function); changes → `setChannelEq` → atomics.
- [ ] **Step 2: Verify by ear + eyes** — sine + pink noise through EQ, drag bell +12 dB at 1k → audible + curve moves; bypass → flat yellow line. Screenshot open window.
- [ ] **Step 3: Commit** `git commit -m "feat(eq): Pro-Q curve editor + knobs"`

### Task 6: Analyzer + persistence wiring + final verify

**Files:**
- Modify: `app/src/EQWindow.*`, `app/src/EQCurveComponent.*` (spectrum background), `app/src/AudioEngine.cpp:656,707` (if not done in Task 3), `core/src/SceneValidator.*` (ranges)

**Interfaces:**
- Consumes: all prior tasks.
- Produces: analyzer toggle `Pre/Post/Off`, validator EQ ranges.

- [ ] **Step 1: Implement analyzer** — `juce::dsp::FFT 2048` Hann on UI timer (30–60 fps, auto-drop to 30 on overload), grey fill behind curve, taps post-EQ buffer copy; never on audio thread; one FFT per open window.
- [ ] **Step 2: Wire persistence + validator** — scene save/load eqBands + eqEnabled; validator: freq 10–22050, gain ±15, Q 0.1–20; v1/v2 → flat defaults.
- [ ] **Step 3: Full gate** Run: `cmake --build build --config Release` (`/W4 /WX` clean), `ctest --test-dir build --output-on-failure` (all green), launch smoke, screenshots `screenshots/rack-1600x900.png` + `rack-1180x700.png` (EQ button + open window), refresh root `PluginRack.exe`.
- [ ] **Step 4: Commit** `git commit -m "feat(eq): analyzer + persistence + verify"`
