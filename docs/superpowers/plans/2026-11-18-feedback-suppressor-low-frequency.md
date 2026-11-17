# Low-Frequency Feedback Suppressor Accuracy — Implementation Plan (PRP)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Status:** Tasks 1-7 implemented and verified on 2026-11-18. All 48 feedback-suppressor test cases and the
full 423-case suite pass; `tools/build-and-copy.ps1` produced `PluginRack.exe`. Measured post-patch numbers are
in the table below. The work is not committed yet because the working tree also carries the unrelated
"speech-safe preset" changes, which share three of the touched files.

**Goal:** Fix the three reproduced low-frequency detection misses — the per-callback gate that wipes the analysis ring, the mono-sum detector input that cancels anti-phase feedback, and the cut budget that lets the high band starve a stronger bass resonance — and stop the half-bin centre flip-flop, without changing any user-facing default.

**Architecture:** Four independent changes inside `FeedbackSuppressorProcessor`, each landed behind its own failing test: (1) a sample-clocked 50 ms rolling energy gate with hysteresis, 120 ms close dwell, a keep-warm ring and a 2 s stale reset; (2) per-channel detector filtering and two analysis rings, with the low band combining `max(|L_k|, |R_k|)` in one packed complex FFT and the high band still reading `0.5*(L+R)`; (3) per-band detection shortlists merged and ranked by `levelDb` before the cut cap, plus preemption of the weakest active cut by a confirmed candidate that is ≥ 6 dB louder; (4) a 5-point log-magnitude vertex for isolated low-band peaks with the existing 3-point/plateau path as fallback.

**Tech Stack:** C++20, MSVC `/W4 /WX`, CMake + Ninja, Catch2 3.7.1. No JUCE in `core`; the processor must stay copyable and allocation-free in `process()`.

**Spec:** `docs/superpowers/specs/2026-11-18-feedback-suppressor-low-frequency-review.md` (review + measured baseline). Original proposals: `plan.md`, `improvement.md`.

## Baseline (measured, `ed6e3b5` tuning)

48 kHz, 1 s, HP 20 Hz, LP 20 kHz, sensitivity −43 dBFS, attenuation 24 dB, multiplier 4, confirm 60 ms,
attack 9 / hold 90 / release 10 ms:

| Probe | Today | Target |
| --- | --- | --- |
| 68 Hz 0.22 in phase, 512-frame blocks | 192.0 ms, 23.98 dB | keep |
| same, right channel inverted | no cut, 0.00 dB | cut within 300 ms, ≥ 12 dB |
| 40 Hz 0.012, 64-frame blocks | no cut, 0.00 dB | cut within 300 ms, ≥ 12 dB |
| 40 Hz 0.012, 1024-frame blocks | 213.3 ms, 23.96 dB | keep |
| 68 Hz 0.32 + 1 kHz 0.06, 1 cut | ends at 999.64 Hz | 68 Hz cut, ≥ 12 dB |
| exact half-bin tones | up to ±2.4 Hz, oscillates | ≤ 1 Hz, no flip |

## Post-patch (measured after Tasks 1-6)

Same probe rig, 48 kHz, 1 s, identical pinned effect, `build/dev` Release build:

| Probe | Before | After |
| --- | --- | --- |
| 68 Hz 0.22 in phase, 512-frame blocks | 192.0 ms, 23.98 dB | 202.7 ms, 23.99 dB |
| same, right channel inverted | no cut, 0.00 dB | 202.7 ms at 67.93 Hz, 23.99 dB |
| 40 Hz 0.012, 64-frame blocks | no cut, 0.00 dB | 208.0 ms at 39.92 Hz, 23.96 dB |
| 40 Hz 0.012, 1024-frame blocks | 213.3 ms, 23.96 dB | 213.3 ms at 39.93 Hz, 23.96 dB |
| 68 Hz 0.32 + 1 kHz 0.06, 1 cut | 999.64 Hz notch, 0.00 dB at 68 Hz | 202.7 ms at 67.93 Hz, 23.99 dB |
| 64.453125 Hz (11 bins, on-bin) | — | 64.45 Hz, 0.00 Hz error |
| 67.3828125 Hz (11.5 bins, exact half-bin) | 65.68 Hz (−1.70 Hz) | 67.58 → 67.42 Hz (≤ 0.20 Hz) |

Half-bin sweep, settled error before → after (48 kHz, 8,192-point low band, 5.86 Hz bins):
38.086 Hz ±1.41 → ±0.14-0.24 Hz; 43.945 ±2.40 → 0.13-0.23; 49.805 ±1.17 → 0.13-0.14;
55.664 ±2.21 → 0.14-0.15; 61.523 ±0.24 → 0.01-0.03; 67.383 ±0.88 → 0.10-0.20;
73.242 ±0.88 → 0.10-0.20; 20 Hz +2.55 → +0.81; 25 Hz +0.09 → 0.09.

Gate behaviour (`fs_probe4`):
- one all-zero 64-sample block inside a 68 Hz tone: `gateCloses` 0, `gateResets` 0, notch held;
- 140 ms pause: notch held through the pause, re-cut one hop (10.7 ms) after the tone returns;
- 2 s of silence: `gateCloses` 1 at 120 ms, window cleared only once the *closed* dwell reaches 2 s
  (≈2.14 s of silence), after which re-acquisition costs a full window fill (202.7 ms).

Two honest notes on the numbers:
- The 68 Hz in-phase probe reads 202.7 ms instead of 192.0 ms. The difference is one polled block:
  the rolling gate opens on the first non-zero sample (not the first block), so the 8,192-sample
  counter crosses its threshold one block later. The underlying detection latency is unchanged.
- The 24 dB attenuation is the existing depth cap; this patch does not change notch depth.

Verification: `build\dev\tests\pluginrack_tests.exe "feedback suppressor*"` → 48 cases, 157 assertions,
all passing; full suite → 423 cases, 57,082 assertions, all passing; `tools\build-and-copy.ps1` produced
`PluginRack.exe` at the repository root.

## Global Constraints

- **Do not change defaults.** `core/include/pluginrack/FeedbackSuppressorEffect.h:15-43` currently holds the
  uncommitted speech-safe preset (HP 90 Hz, attenuation 12, multiplier 1, confirm 300 ms, 4 cuts). Tests
  pin the detector fields they need; the patch must not touch these values, `kFeedbackSuppressorMaxAttenuationDb`
  (24 dB), `kAnalysisSplitHz` (500 Hz), `kMaxNotches` (8), or any scene field.
- **No allocation, no locks, no logging in `process()`.** Every buffer is sized in `prepare()`; all new
  members are copied in the copy constructor and copy assignment
  (`core/src/FeedbackSuppressorEffect.cpp:66-125`).
- **Keep the existing envelope, hold/release, depth mapping, and UI behaviour.** Only detection changes.
- **The working tree is dirty** with the user's reset-on-open / preset / info-label work. Confirm with the
  human whether that is committed first; when staging, add only this task's files or hunks. Never revert it.
- **After every task that changes `core/` or `app/` sources**, run
  `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1` and confirm `PluginRack.exe` in the
  repo root is fresh (AGENTS.md).
- Commit with Conventional Commits and no AI-attribution trailers.

**Build environment.** `cmake` is not on `PATH` in this shell. Run test builds through the Developer
environment once per shell:

```powershell
& $env:ComSpec /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && cd /d "C:\Users\thorz\OneDrive\Desktop\PluginRack" && "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build\dev --target pluginrack_tests --parallel 4'
```

The short forms below assume that environment (`cmake --build build/dev --target pluginrack_tests`).
Run the suite with `build\dev\tests\pluginrack_tests.exe "feedback suppressor*"`.

## Review Focus

1. **Near-threshold LF tone in tiny host blocks.** A 64-frame block near a zero crossing must not close the
   gate or clear the ring. Pinned by Task 2's 40 Hz/64-frame probe and the dip probe.
2. **Anti-phase stereo feedback.** Both channels loud, opposite polarity, must still produce one linked cut.
   Pinned by Task 3's inverted probe and the 90° case.
3. **A stronger bass resonance behind a full cut bank.** The 68 Hz + 1 kHz / one-cut mixture must end on
   68 Hz, while a merely louder neighbour must not steal a healthy cut. Pinned by Task 4's two tests.
4. **Exact half-bin tones.** Centres must stop flipping by ±0.5 bin and settle within 1 Hz. Pinned by
   Task 5's four half-bin cases plus a clipped-peak fallback case.
5. **Silence and transport stops.** Stale candidates must not survive 2 s of closed gate, and a dip must not
   erase confirmation progress. Pinned by Task 2's stale-reset test and the existing hold/release tests.

---

### Task 1: Probe harness and the six documented regressions

**Files:**
- Modify: `tests/test_feedback_suppressor.cpp` (append helpers inside the existing anonymous namespace at
  `:11-27`, add test cases after the last case)
- No production code.

**Interfaces:**
- Produces (test-local): `pluginrack::FeedbackSuppressorEffect pinnedLowFrequencyEffect(int cuts)`,
  `ProbeRun runProbe(std::vector<float> left, std::vector<float> right, int blockFrames, int cuts, float probeHz)`,
  `float probeAttenuationDb(const std::vector<float>& input, const std::vector<float>& output, double hz)`,
  and `void mixInto(std::vector<float>& target, const std::vector<float>& other)`.

- [ ] **Step 1: Add the helpers and the tests**

Inside the existing `namespace { ... }` in `tests/test_feedback_suppressor.cpp`, after `rms()`:

```cpp
constexpr double kProbeRate = 48000.0;

pluginrack::FeedbackSuppressorEffect pinnedLowFrequencyEffect(int cuts) {
    auto effect = pluginrack::makeDefaultFeedbackSuppressorEffect();
    effect.bypassed = false;
    effect.strength = pluginrack::feedbackSuppressorStrengthForCuts(cuts);
    effect.detectorHighPassHz = 20.0f;
    effect.detectorLowPassHz = 20000.0f;
    effect.inputSensitivityDb = -43.0f;
    effect.attenuationDb = pluginrack::kFeedbackSuppressorMaxAttenuationDb;
    effect.gainReductionMultiplier = pluginrack::kFeedbackSuppressorMaxGainReductionMultiplier;
    effect.detectionDelayMs = 60.0f;
    effect.attackMs = 9.0f;
    effect.holdMs = 90.0f;
    effect.releaseMs = 10.0f;
    return effect;
}

struct ProbeRun {
    std::vector<float> left;
    std::vector<float> right;
    float firstCutMs = -1.0f;
    float firstCutHz = 0.0f;
    float lastCutHz = 0.0f;
};

ProbeRun runProbe(std::vector<float> left, std::vector<float> right, int blockFrames,
                  int cuts, float probeHz) {
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(kProbeRate);
    processor.setEffect(pinnedLowFrequencyEffect(cuts));
    ProbeRun run{std::move(left), std::move(right)};

    std::array<float, 8> freqs{};
    std::array<float, 8> depths{};
    const int total = static_cast<int>(run.left.size());
    for (int offset = 0; offset < total; offset += blockFrames) {
        const int frames = std::min(blockFrames, total - offset);
        processor.process(run.left.data() + offset, run.right.data() + offset, frames);
        const int count = processor.copyActiveNotches(freqs.data(), depths.data(), 8);
        for (int i = 0; i < count; ++i) {
            if (std::fabs(freqs[static_cast<std::size_t>(i)] - probeHz) > 5.0f) continue;
            run.lastCutHz = freqs[static_cast<std::size_t>(i)];
            if (run.firstCutMs < 0.0f) {
                run.firstCutMs = 1000.0f * static_cast<float>(offset + frames)
                               / static_cast<float>(kProbeRate);
                run.firstCutHz = freqs[static_cast<std::size_t>(i)];
            }
        }
    }
    return run;
}

double projectedAmplitude(const std::vector<float>& samples, std::size_t from, double hz) {
    const std::size_t count = samples.size() - from;
    double real = 0.0;
    double imag = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * hz
                           * static_cast<double>(i) / kProbeRate;
        real += static_cast<double>(samples[from + i]) * std::cos(phase);
        imag += static_cast<double>(samples[from + i]) * std::sin(phase);
    }
    return 2.0 * std::sqrt(real * real + imag * imag)
         / static_cast<double>(std::max<std::size_t>(1, count));
}

float probeAttenuationDb(const std::vector<float>& input, const std::vector<float>& output,
                         double hz) {
    const std::size_t tail = static_cast<std::size_t>(0.5 * kProbeRate);
    const std::size_t from = output.size() > tail ? output.size() - tail : 0;
    const double in = projectedAmplitude(input, from, hz);
    const double out = projectedAmplitude(output, from, hz);
    return static_cast<float>(20.0 * std::log10(std::max(in, 1e-12) / std::max(out, 1e-12)));
}

void mixInto(std::vector<float>& target, const std::vector<float>& other) {
    for (std::size_t i = 0; i < target.size(); ++i) target[i] += other[i];
}
```

Then append:

```cpp
TEST_CASE("feedback suppressor cuts a 68 Hz tone within 300 ms") {
    auto tone = sine(68.0f, 0.22f, 48000);
    const auto run = runProbe(tone, tone, 512, 8, 68.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(run.firstCutMs <= 300.0f);
    REQUIRE(probeAttenuationDb(tone, run.left, 68.0) >= 12.0f);
}

TEST_CASE("feedback suppressor cuts anti-phase low-frequency feedback") {
    auto left = sine(68.0f, 0.22f, 48000);
    auto right = left;
    for (float& sample : right) sample = -sample;
    const auto run = runProbe(left, right, 512, 8, 68.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(run.firstCutMs <= 300.0f);
    REQUIRE(probeAttenuationDb(left, run.left, 68.0) >= 12.0f);
}

TEST_CASE("feedback suppressor cuts a quiet 40 Hz tone in 64-frame blocks") {
    auto tone = sine(40.0f, 0.012f, 48000);
    const auto run = runProbe(tone, tone, 64, 8, 40.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(run.firstCutMs <= 300.0f);
    REQUIRE(probeAttenuationDb(tone, run.left, 40.0) >= 12.0f);
}

TEST_CASE("feedback suppressor cuts a quiet 40 Hz tone in 1024-frame blocks") {
    auto tone = sine(40.0f, 0.012f, 48000);
    const auto run = runProbe(tone, tone, 1024, 8, 40.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(run.firstCutMs <= 300.0f);
    REQUIRE(probeAttenuationDb(tone, run.left, 40.0) >= 12.0f);
}

TEST_CASE("feedback suppressor spends its only cut on the stronger bass tone") {
    auto tone = sine(68.0f, 0.32f, 48000);
    mixInto(tone, sine(1000.0f, 0.06f, 48000));
    const auto run = runProbe(tone, tone, 512, 1, 68.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(run.firstCutMs <= 400.0f);
    REQUIRE(probeAttenuationDb(tone, run.left, 68.0) >= 12.0f);
}

TEST_CASE("feedback suppressor keeps its history across a quiet block") {
    auto tone = sine(68.0f, 0.3f, 48000);
    std::vector<float> left(tone.begin(), tone.begin() + 24000);
    std::vector<float> right = left;
    // 64 frames of digital silence inside the first fill must not restart the
    // 8192-sample analysis window.
    std::fill(left.begin() + 7200, left.begin() + 7264, 0.0f);
    right = left;
    const auto run = runProbe(left, right, 512, 8, 68.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(run.firstCutMs <= 260.0f);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build/dev --target pluginrack_tests --parallel 4` then
`build\dev\tests\pluginrack_tests.exe "feedback suppressor*"`.

Expected: FAIL on `cuts anti-phase` (count 0), `cuts a quiet 40 Hz tone in 64-frame blocks` (count 0),
`spends its only cut on the stronger bass tone` (no 68 Hz cut), and `keeps its history across a quiet block`
(first cut ≈ 340 ms). The 68 Hz, 40 Hz/1024 and existing cases stay green.

- [ ] **Step 3: Commit**

`test(feedback-suppressor): pin the six low-frequency probes`

---

### Task 2: Sample-clocked gate with keep-warm history

**Files:**
- Modify: `core/include/pluginrack/FeedbackSuppressorEffect.h` (constants near `:13-44`, state near `:195-243`,
  private methods near `:245-257`)
- Modify: `core/src/FeedbackSuppressorEffect.cpp` (`prepare` `:152-180`, copy ops `:66-125`,
  `setEffect` `:201-244`, `clearAnalysisHistory` `:279-291`, `reset` `:378-408`, `process` `:945-1055`)

**Interfaces:**
- Produces: `inline constexpr float kFeedbackSuppressorGateWindowSeconds = 0.05f;`,
  `kFeedbackSuppressorGateCloseRatio = 0.70794578f` (−3 dB), `kFeedbackSuppressorGateCloseSeconds = 0.12f`,
  `kFeedbackSuppressorGateResetSeconds = 2.0f`, `kFeedbackSuppressorMaxGateWindowSamples = 16384`.
- Produces (private): `void refreshGateThresholds() noexcept;`, `void resetGate() noexcept;`.
- Consumes: `sensitivityLinear_`, `sampleRate_`.

- [ ] **Step 1: Add the failing gate tests**

Append to `tests/test_feedback_suppressor.cpp`:

```cpp
TEST_CASE("feedback suppressor does not close its gate on one quiet block") {
    auto tone = sine(68.0f, 0.22f, 48000);
    const auto run = runProbe(tone, tone, 64, 8, 68.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(run.firstCutMs <= 240.0f);
}

TEST_CASE("feedback suppressor forgets the detector after two seconds of silence") {
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(pinnedLowFrequencyEffect(8));

    auto tone = sine(68.0f, 0.3f, 19200);
    auto right = tone;
    for (std::size_t offset = 0; offset < tone.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, tone.size() - offset));
        processor.process(tone.data() + offset, right.data() + offset, frames);
    }
    std::array<float, 8> notches{};
    REQUIRE(processor.copyActiveNotches(notches.data(), 8) > 0);

    std::vector<float> silence(48000 * 3, 0.0f);
    std::vector<float> silenceRight = silence;
    for (std::size_t offset = 0; offset < silence.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, silence.size() - offset));
        processor.process(silence.data() + offset, silenceRight.data() + offset, frames);
    }
    REQUIRE(processor.copyActiveNotches(notches.data(), 8) == 0);
}
```

- [ ] **Step 2: Run to verify the first fails**

Run: `build\dev\tests\pluginrack_tests.exe "feedback suppressor does not close its gate*"`.
Expected: FAIL (the 64-frame case still misses, because `process()` clears the ring on a quiet block).

- [ ] **Step 3: Implement the gate**

Header, next to the other `kFeedback*` constants:

```cpp
// The detector gate is sample-clocked, not callback-clocked: a 64-frame host
// block near a low tone's zero crossing must not look like silence.
inline constexpr float kFeedbackSuppressorGateWindowSeconds = 0.05f;
inline constexpr float kFeedbackSuppressorGateCloseRatio = 0.70794578f;  // -3 dB
inline constexpr float kFeedbackSuppressorGateCloseSeconds = 0.12f;
inline constexpr float kFeedbackSuppressorGateResetSeconds = 2.0f;
inline constexpr int kFeedbackSuppressorMaxGateWindowSamples = 16384;
```

Private state, after `bool gateOpen_ = false;`:

```cpp
    std::vector<float> gateEnergy_;
    int gateWrite_ = 0;
    int gateWindow_ = 1;
    double gateEnergySum_ = 0.0;
    double gateOpenEnergy_ = 0.0;
    double gateCloseEnergy_ = 0.0;
    int gateCloseDwell_ = 0;
    int gateResetDwell_ = 0;
    int gateClosedSamples_ = 0;
```

Private methods: `void refreshGateThresholds() noexcept;` and `void resetGate() noexcept;`.

`prepare()`: before `setEffect(effect_)`:

```cpp
    gateWindow_ = std::clamp(
        static_cast<int>(std::lround(sampleRate * static_cast<double>(kFeedbackSuppressorGateWindowSeconds))),
        1, kFeedbackSuppressorMaxGateWindowSamples);
    gateEnergy_.assign(static_cast<std::size_t>(gateWindow_), 0.0f);
    resetGate();
```

`setEffect()`: after `sensitivityLinear_` is recomputed, call `refreshGateThresholds();`.

```cpp
void FeedbackSuppressorProcessor::refreshGateThresholds() noexcept {
    const double open = static_cast<double>(sensitivityLinear_) * sensitivityLinear_;
    gateOpenEnergy_ = open * 2.0 * static_cast<double>(gateWindow_);
    gateCloseEnergy_ = gateOpenEnergy_ * static_cast<double>(kFeedbackSuppressorGateCloseRatio)
                     * static_cast<double>(kFeedbackSuppressorGateCloseRatio);
    gateCloseDwell_ = std::max(1, static_cast<int>(std::lround(
        sampleRate_ * static_cast<double>(kFeedbackSuppressorGateCloseSeconds))));
    gateResetDwell_ = std::max(1, static_cast<int>(std::lround(
        sampleRate_ * static_cast<double>(kFeedbackSuppressorGateResetSeconds))));
}

void FeedbackSuppressorProcessor::resetGate() noexcept {
    std::fill(gateEnergy_.begin(), gateEnergy_.end(), 0.0f);
    gateEnergySum_ = 0.0;
    gateWrite_ = 0;
    gateClosedSamples_ = 0;
    gateOpen_ = false;
}
```

Copy constructor and assignment: copy `gateEnergy_`, `gateWrite_`, `gateWindow_`, `gateEnergySum_`,
`gateOpenEnergy_`, `gateCloseEnergy_`, `gateCloseDwell_`, `gateResetDwell_`, `gateClosedSamples_`.

`process()`: delete the block-RMS gate and the two early returns (`:945-972`, including the
`clearAnalysisHistory()` on close). Inside the per-sample loop, right after `inL`/`inR` are fetched, add:

```cpp
            const double instantEnergy = static_cast<double>(inL) * inL
                                       + static_cast<double>(inR) * inR;
            gateEnergySum_ -= static_cast<double>(gateEnergy_[static_cast<std::size_t>(gateWrite_)]);
            gateEnergy_[static_cast<std::size_t>(gateWrite_)] = static_cast<float>(instantEnergy);
            gateEnergySum_ += instantEnergy;
            gateWrite_ = (gateWrite_ + 1) % gateWindow_;
            if (gateOpen_) {
                if (gateEnergySum_ < gateCloseEnergy_) {
                    if (++gateClosedSamples_ >= gateCloseDwell_) gateOpen_ = false;
                } else {
                    gateClosedSamples_ = 0;
                }
            } else if (gateEnergySum_ >= gateOpenEnergy_) {
                gateOpen_ = true;
                gateClosedSamples_ = 0;
            } else if (++gateClosedSamples_ >= gateResetDwell_) {
                clearAnalysisHistory();
                gateClosedSamples_ = 0;
            }
```

Then:
- change the analysis feed condition `if (detectorOpen)` at `:1015` to `if (gateOpen_)`;
- change the per-chunk branch at `:1045-1055` to test `gateOpen_` and, while closed, call
  `updateNotches({}, false)` so hold/release still run but candidates are not aged;
- delete `gateOpen_ = detectorOpen;`;
- `reset()`: call `resetGate()` in addition to `closeGate()`;
- `closeGate()`: set `gateClosedSamples_ = 0`.

`updateNotches` gains a defaulted parameter:
`void updateNotches(const std::vector<Detection>& detections, bool ageCandidates = true);`
and the aging loop at `:804-809` only runs when `ageCandidates` is true.

- [ ] **Step 4: Run to verify green**

Run: `build\dev\tests\pluginrack_tests.exe "feedback suppressor*"`.
Expected: all green, including the new gate cases and every existing gate/hold/release case.

- [ ] **Step 5: Build and copy the app, then commit**

`powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`
then `fix(feedback-suppressor): sample-clock the detector gate`

---

### Task 3: Stereo-safe low-band analysis

**Files:**
- Modify: `core/include/pluginrack/FeedbackSuppressorEffect.h` (`:195-243` state, copy ops in the source)
- Modify: `core/src/FeedbackSuppressorEffect.cpp` (`prepare` `:152-180`, `setEffect`/`reset` filter calls,
  `clearAnalysisHistory` `:279-291`, `resetAnalysisHighPass`/`LowPass`, `analyze` `:410-473`,
  `process` `:1013-1036`)

**Interfaces:**
- Produces: analysis ring `std::array<std::vector<float>, 2> analysisBuffer_` (index 0 = left, 1 = right),
  per-channel filter state `std::array<std::array<float, 4>, 2> hpZ1_, hpZ2_, lpZ1_, lpZ2_`.
- Consumes: the existing `fft(std::vector<float>&, std::vector<float>&)` and `magsLow_`.

- [ ] **Step 1: Extend the regression tests**

```cpp
TEST_CASE("feedback suppressor cuts a 90 degree phase-shifted bass tone") {
    auto left = sine(68.0f, 0.22f, 48000);
    std::vector<float> right(left.size());
    for (std::size_t i = 0; i < left.size(); ++i) {
        right[i] = static_cast<float>(0.22 * std::sin(
            2.0 * 3.14159265358979323846 * 68.0 * static_cast<double>(i) / 48000.0
            + 0.5 * 3.14159265358979323846));
    }
    const auto run = runProbe(left, right, 512, 8, 68.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(probeAttenuationDb(left, run.left, 68.0) >= 12.0f);
}

TEST_CASE("feedback suppressor keeps an in-phase bass cut centered") {
    auto tone = sine(68.0f, 0.22f, 48000);
    const auto run = runProbe(tone, tone, 512, 8, 68.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(std::fabs(run.firstCutHz - 68.0f) <= 3.0f);
}
```

- [ ] **Step 2: Run to verify the anti-phase case still fails**

Run: `build\dev\tests\pluginrack_tests.exe "feedback suppressor*anti-phase*"` and the Phase 1 case.
Expected: FAIL (no notch), while the 90° case passes (its mono sum is not zero).

- [ ] **Step 3: Implement per-channel analysis**

Header: replace `std::vector<float> analysisBuffer_;` with
`std::array<std::vector<float>, 2> analysisBuffer_;` and the four state arrays with
`std::array<std::array<float, 4>, 2> hpZ1_{}, hpZ2_{}, lpZ1_{}, lpZ2_{};` (coefficients stay shared).

`prepare()`: `analysisBuffer_[0].assign(kLowAnalysisSize, 0.0f);` and the same for `[1]`.

`clearAnalysisHistory()`: zero both rings; reset all per-channel states.

`process()` per sample, replacing `:1013-1036`:

```cpp
            if (gateOpen_) {
                float filtered[2] = {inL, inR};
                for (int channel = 0; channel < 2; ++channel) {
                    float hp = filtered[channel];
                    for (int stage = 0; stage < 4; ++stage) {
                        const auto s = static_cast<std::size_t>(stage);
                        const float y = hpB0_[s] * hp + hpZ1_[static_cast<std::size_t>(channel)][s];
                        hpZ1_[static_cast<std::size_t>(channel)][s] =
                            hpB1_[s] * hp - hpA1_[s] * y + hpZ2_[static_cast<std::size_t>(channel)][s];
                        hpZ2_[static_cast<std::size_t>(channel)][s] =
                            hpB2_[s] * hp - hpA2_[s] * y;
                        hp = y;
                    }
                    float lp = hp;
                    for (int stage = 0; stage < 4; ++stage) {
                        const auto s = static_cast<std::size_t>(stage);
                        const float y = lpB0_[s] * lp + lpZ1_[static_cast<std::size_t>(channel)][s];
                        lpZ1_[static_cast<std::size_t>(channel)][s] =
                            lpB1_[s] * lp - lpA1_[s] * y + lpZ2_[static_cast<std::size_t>(channel)][s];
                        lpZ2_[static_cast<std::size_t>(channel)][s] =
                            lpB2_[s] * lp - lpA2_[s] * y;
                        lp = y;
                    }
                    filtered[channel] = lp;
                }
                for (int channel = 0; channel < 2; ++channel) {
                    analysisBuffer_[static_cast<std::size_t>(channel)]
                                   [static_cast<std::size_t>(analysisWrite_)] = filtered[channel];
                }
                analysisWrite_ = (analysisWrite_ + 1) % kLowAnalysisSize;
                if (analysisCount_ < kLowAnalysisSize) ++analysisCount_;
            }
```

`analyze()` high band: read the mono sum from the two rings
(`sample = 0.5f * (left[src] + right[src])`).

`analyze()` low band: fill the packed complex input and unpack after the transform:

```cpp
        for (int i = 0; i < n; ++i) {
            const int src = (analysisWrite_ - n + i + kLowAnalysisSize) % kLowAnalysisSize;
            const auto index = static_cast<std::size_t>(src);
            const float w = 0.5f - 0.5f * std::cos(
                2.0f * kPi * static_cast<float>(i) / static_cast<float>(n - 1));
            fftLowReal_[static_cast<std::size_t>(i)] = analysisBuffer_[0][index] * w;
            fftLowImag_[static_cast<std::size_t>(i)] = analysisBuffer_[1][index] * w;
        }
        fft(fftLowReal_, fftLowImag_);
        for (int i = 1; i < n / 2; ++i) {
            const int mirror = n - i;
            const float xr = fftLowReal_[static_cast<std::size_t>(i)];
            const float xi = fftLowImag_[static_cast<std::size_t>(i)];
            const float yr = fftLowReal_[static_cast<std::size_t>(mirror)];
            const float yi = fftLowImag_[static_cast<std::size_t>(mirror)];
            const float leftRe = 0.5f * (xr + yr);
            const float leftIm = 0.5f * (xi - yi);
            const float rightRe = 0.5f * (xi + yi);
            const float rightIm = -0.5f * (xr - yr);
            magsLow_[static_cast<std::size_t>(i)] = std::max(
                std::sqrt(leftRe * leftRe + leftIm * leftIm),
                std::sqrt(rightRe * rightRe + rightIm * rightIm));
        }
```

This uses the same single 8192-point transform as today: left in the real part, right in the imaginary
part, then split with the Hermitian pair. No extra FFT, no extra allocation.

- [ ] **Step 4: Run the full feedback suite**

Expected: green, including in-phase, anti-phase, 90°, and all existing single-channel cases.

- [ ] **Step 5: Build and copy, then commit**

`fix(feedback-suppressor): detect low-band feedback per channel`

---

### Task 4: Rank both bands before the cap, and preempt a weaker cut

**Files:**
- Modify: `core/include/pluginrack/FeedbackSuppressorEffect.h` (`Notch` `:149-162`, `analyzeSpectrum`
  signature `:246-247`, `prepare` reserve)
- Modify: `core/src/FeedbackSuppressorEffect.cpp` (`prepare` `:152-180`, `analyze` `:410-473`,
  `analyzeSpectrum` `:475-655`, `updateNotches` `:657-895`)

**Interfaces:**
- `Notch` gains `float levelDb = -120.0f;`.
- `void analyzeSpectrum(int fftSize, const std::vector<float>& mags, float bandLowHz, float bandHighHz, int budget);`
- `prepare()`: `detections_.reserve(2 * kMaxNotches);`

- [ ] **Step 1: Add the negative preemption test**

```cpp
TEST_CASE("feedback suppressor keeps a cut when a new peak is only slightly louder") {
    auto tone = sine(1000.0f, 0.3f, 48000);
    std::vector<float> left = tone;
    std::vector<float> right = tone;
    const int switchAt = 14400;              // 300 ms: the 1 kHz cut is established
    for (int i = switchAt; i < static_cast<int>(left.size()); ++i) {
        const double t = static_cast<double>(i - switchAt) / 48000.0;
        left[static_cast<std::size_t>(i)] += static_cast<float>(
            0.36 * std::sin(2.0 * 3.14159265358979323846 * 1500.0 * t));
    }
    right = left;
    const auto run = runProbe(left, right, 512, 1, 1000.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(std::fabs(run.lastCutHz - 1000.0f) <= 20.0f);
}
```

- [ ] **Step 2: Run to verify the Phase 1 mixture test still fails**

Run: `build\dev\tests\pluginrack_tests.exe "feedback suppressor*only cut*"`.
Expected: FAIL — the single slot ends on 999.64 Hz.

- [ ] **Step 3: Rank, merge, then cap**

`analyze()`:

```cpp
    detections_.clear();
    const int maxCuts = std::min(kMaxNotches,
                                 feedbackSuppressorCutCount(effect_.strength));
    // ... high band, then low band, each called with maxCuts as its budget ...
    std::sort(detections_.begin(), detections_.end(),
              [](const Detection& a, const Detection& b) {
                  return a.levelDb > b.levelDb;
              });
    int kept = 0;
    for (int i = 0; i < static_cast<int>(detections_.size())
                 && kept < maxCuts; ++i) {
        bool duplicate = false;
        for (int j = 0; j < kept; ++j) {
            const float bin = std::max(detections_[static_cast<std::size_t>(i)].binWidthHz,
                                       detections_[static_cast<std::size_t>(j)].binWidthHz);
            if (std::fabs(detections_[static_cast<std::size_t>(i)].freq
                          - detections_[static_cast<std::size_t>(j)].freq) <= bin * 1.25f) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) detections_[static_cast<std::size_t>(kept++)] = detections_[static_cast<std::size_t>(i)];
    }
    detections_.resize(static_cast<std::size_t>(kept));
    updateNotches(detections_);
```

`analyzeSpectrum()`: take the fifth `int budget` parameter, delete the local
`maxCuts` definition at `:595`, and replace `if (detections_.size() >= maxCuts) break;` with
`if (++added > budget) break;` (increment after each successful push).

- [ ] **Step 4: Preempt by level**

In `updateNotches`, set `levelDb` on match (`:716-733`), on merge (`:857-860`), and in
`makeNotchFromCandidate`. Replace the replacement block at `:877-894` with:

```cpp
        if (effect_.holdMs <= 0.0f) continue;
        if (candidate.tonalityDb < 8.0f) continue;
        int replaceIndex = -1;
        float weakestLevel = 1.0e9f;
        for (int j = 0; j < count; ++j) {
            const auto& held = next[static_cast<std::size_t>(j)];
            if (held.levelDb < weakestLevel) {
                weakestLevel = held.levelDb;
                replaceIndex = j;
            }
        }
        if (replaceIndex < 0
            || candidate.lastLevelDb < weakestLevel + 6.0f) continue;

        Notch notch = makeNotchFromCandidate(candidate);
        notch.holdSamples = holdSamples_;
        updateNotchCoefficients(notch);
        next[static_cast<std::size_t>(replaceIndex)] = notch;
        candidate = Candidate{};
```

`makeNotchFromCandidate` sets `notch.levelDb = candidate.lastLevelDb;`.

- [ ] **Step 5: Run to verify green**

Expected: the mixture test cuts 68 Hz; the "slightly louder" test keeps 1 kHz; every existing
cut-count, hold, and close-peak case stays green.

- [ ] **Step 6: Build and copy, then commit**

`fix(feedback-suppressor): rank and preempt cuts across bands`

---

### Task 5: Five-point vertex for isolated low-band peaks

**Files:**
- Modify: `core/src/FeedbackSuppressorEffect.cpp` (`analyzeSpectrum` interpolation `:570-581`)

**Interfaces:**
- No public API change. `Detection::freq` remains the only output.

- [ ] **Step 1: Add the accuracy tests**

```cpp
namespace {
float halfBinCenterError(float hz) {
    auto tone = sine(hz, 0.3f, 96000);
    const auto run = runProbe(tone, tone, 512, 8, hz);
    REQUIRE(run.firstCutMs > 0.0f);
    return std::fabs(run.lastCutHz - hz);
}
} // namespace

TEST_CASE("feedback suppressor centers low cuts within 1 Hz on half-bin tones") {
    const double bin = kProbeRate / 8192.0;
    for (const double index : {6.5, 7.5, 10.5, 11.5}) {
        const auto hz = static_cast<float>(index * bin);
        INFO("tone " << hz << " Hz");
        REQUIRE(halfBinCenterError(hz) <= 1.0f);
    }
}

TEST_CASE("feedback suppressor still centers a clipped bass peak") {
    auto tone = sine(90.0f, 1.5f, 48000);
    for (float& sample : tone) sample = std::clamp(sample, -1.0f, 1.0f);
    const auto run = runProbe(tone, tone, 512, 8, 90.0f);
    REQUIRE(run.firstCutMs > 0.0f);
    REQUIRE(std::fabs(run.lastCutHz - 90.0f) <= 3.0f);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `build\dev\tests\pluginrack_tests.exe "feedback suppressor*centers low cuts*"`.
Expected: FAIL at 43.945 Hz (settled error 2.40 Hz) and 55.664 Hz (2.21 Hz).

- [ ] **Step 3: Implement the fit**

Replace the interpolation block with:

```cpp
        float offset = 0.0f;
        bool fitted = false;
        const bool plateau = mags[i - 1] > center * 0.95f || mags[i + 1] > center * 0.95f;
        // A two-bin plateau is the exact half-bin case: the picker keeps the
        // upper bin, so a 3-point parabola reports the wrong side every pass.
        // A 5-point least-squares vertex is symmetric across that plateau and
        // removes the +/-0.5-bin flip.
        if (fftSize == kLowAnalysisSize && i >= 2 && i + 2 < n / 2
            && mags[i - 2] < center * 0.5f && mags[i + 2] < center * 0.5f) {
            const float y0 = std::log(std::max(mags[i - 2], 1.0e-20f));
            const float y1 = std::log(std::max(mags[i - 1], 1.0e-20f));
            const float y2 = std::log(std::max(center, 1.0e-20f));
            const float y3 = std::log(std::max(mags[i + 1], 1.0e-20f));
            const float y4 = std::log(std::max(mags[i + 2], 1.0e-20f));
            const float b = (-2.0f * y0 - y1 + y3 + 2.0f * y4) / 10.0f;
            const float a = ((4.0f * y0 + y1 + y3 + 4.0f * y4) - 2.0f * (y0 + y1 + y2 + y3 + y4)) / 14.0f;
            if (a < -1.0e-4f) {
                offset = std::clamp(-b / (2.0f * a), -1.0f, 1.0f);
                fitted = true;
            }
        }
        if (!fitted && !plateau) {
            // existing 3-point log-parabolic interpolation
        }
```

- [ ] **Step 4: Run the suite**

Expected: all four half-bin tones within 1 Hz; the clipped-peak case still within 3 Hz; the existing
68/90/35 Hz and 1 kHz cases unchanged.

- [ ] **Step 5: Build and copy, then commit**

`fix(feedback-suppressor): fit low-band peaks over five bins`

---

### Task 6: Detector counters for tests and tuning

**Files:**
- Modify: `core/include/pluginrack/FeedbackSuppressorEffect.h` (public nested struct + getter, `:118-135`)
- Modify: `core/src/FeedbackSuppressorEffect.cpp` (`prepare`, `process`, `updateNotches`)

**Interfaces:**
- Produces: `struct FeedbackSuppressorStats { std::uint32_t gateCloses = 0; gateResets = 0; promotions = 0; preemptions = 0; candidateRejections = 0; };` and
  `const FeedbackSuppressorStats& stats() const noexcept;`

- [ ] **Step 1: Add the failing test**

```cpp
TEST_CASE("feedback suppressor counts a gate close and a stale reset") {
    pluginrack::FeedbackSuppressorProcessor processor;
    processor.prepare(48000.0);
    processor.setEffect(pinnedLowFrequencyEffect(8));
    std::vector<float> silence(48000 * 3, 0.0f);
    std::vector<float> silenceRight = silence;
    for (std::size_t offset = 0; offset < silence.size(); offset += 512) {
        const int frames = static_cast<int>(std::min<std::size_t>(512, silence.size() - offset));
        processor.process(silence.data() + offset, silenceRight.data() + offset, frames);
    }
    REQUIRE(processor.stats().gateCloses >= 1);
    REQUIRE(processor.stats().gateResets >= 1);
}
```

- [ ] **Step 2: Run to verify it fails to compile**

Expected: `no member named 'stats'`.

- [ ] **Step 3: Implement**

Add the struct and getter; a member `FeedbackSuppressorStats stats_{};` (plain `uint32_t`, test-only,
single-threaded); reset it in `prepare()`; increment `gateCloses` when the gate transitions open→closed,
`gateResets` in the 2 s branch, `promotions` on the `count < maxCuts` path, `preemptions` on replacement,
and `candidateRejections` once per candidate that fails `candidateIsEligible`. Copy it in the copy ops.

- [ ] **Step 4: Run, build, commit**

`build\dev\tests\pluginrack_tests.exe "feedback suppressor*"` green, then
`powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`, then
`feat(feedback-suppressor): expose detector counters`

---

### Task 7: Full verification and follow-up notes

**Files:**
- Modify: `docs/superpowers/specs/2026-11-18-feedback-suppressor-low-frequency-review.md` (append measured
  post-patch numbers to the baseline table)

- [ ] **Step 1: Run the whole core suite**

`build\dev\tests\pluginrack_tests.exe` (no filter). Expected: every case green, no new failures outside
the suppressor.

- [ ] **Step 2: Run the app build and copy**

`powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`, then confirm
`PluginRack.exe` in the repo root has a new timestamp.

- [ ] **Step 3: Record the new probe numbers**

Re-run the six probes (or read the new test assertions) and add a "post-patch" column: time to cut,
attenuation, final centre. Report medians and the hardware-independent facts only; do not claim acoustic
gain before feedback.

- [ ] **Step 4: Commit**

`docs(feedback-suppressor): record the low-frequency probe results`

---

## Follow-ups (explicitly out of this patch)

1. Lower the default detector HP for channel strips (90 Hz today blocks the entire 20–90 Hz band out of
   the box) once the false-notch corpus exists.
2. Mid band (4096 samples, 250–1000 Hz) or a split at 250 Hz for the mud zone.
3. 36 dB cuts below 250 Hz, with the UI knob change and scene compatibility.
4. Provisional 4096 detector with 3 dB cuts pending confirmation (only if 300 ms is still too slow).
5. Period-aware attack/hold for LF notches, and double-precision biquads if 20–30 Hz measurement shows drift.
6. Frequency-aware tonality/prominence, growth pre-trigger, adaptive confirm delay — each only with corpus
   evidence that the simpler path underperforms.
7. Aggregate `copyActiveNotches` across all chain suppressors (`app/src/AudioEngine.cpp:4798-4820`).
8. Ring-out / latched notches, RTA band overlay, scene field for an LF mode.

## Self-review notes

- Every one of the three documented misses maps to one task (2, 3, 4), and each task starts from a test
  that fails on `ed6e3b5` behaviour and on the current working tree.
- No task changes a default, a scene field, the UI, or an envelope; the cut-count cap, bypass
  transparency, and saved-scene behaviour are untouched.
- Allocation discipline: the gate ring, the second analysis ring, and the `2*kMaxNotches` detection
  reserve are all sized in `prepare()`; the in-place dedupe and `resize` only shrink.
- Type consistency: `levelDb` is `float` on `Notch`, matching `Detection`; counters are `std::uint32_t`.
