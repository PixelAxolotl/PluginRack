# Low-frequency feedback suppressor: review of the improvement plans against the code

**Reviewed commit:** `ed6e3b507bec4c30c3dba9cf29ed073f527565ec`
**Reviewed working tree:** `ed6e3b5` + uncommitted speech-safe-preset work (defaults, reset-on-open, info label)
**Date:** 2026-11-18
**Documents reviewed:** `plan.md` ("Low-frequency feedback improvement plan") and `improvement.md` ("accuracy & low-frequency improvement plan")

## Verdict

Both documents describe the right three bugs. All three reproduce exactly on this checkout, and each is
isolated to a single code path by a one-variable experiment. The proposed fixes are directionally correct.

The plan is too large for the evidence, though: three of its "root causes" are contradicted by the code,
one acceptance gate cannot be met by the proposed FFT size, and one proposal (36 dB cuts) contradicts the
plan's own validation table. The smallest patch that fixes every reproduced miss is four changes, not the
full P0/P1/P2 list:

1. sample-clocked rolling gate with keep-warm analysis history;
2. per-channel low-band spectra (`max(|L|, |R|)`);
3. cross-band ranking **plus** preemption of a weaker active cut;
4. a sub-bin peak fit good enough to stop the half-bin flip-flop.

Everything else in the documents is follow-up work, not a prerequisite.

## 1. Reproduction (measured, not quoted)

Six offline probes, 48 kHz, 1 s, effect pinned to the `ed6e3b5` tuning (HP 20 Hz, LP 20 kHz,
sensitivity −43 dBFS, attenuation 24 dB, multiplier 4, confirm 60 ms, attack 9 / hold 90 / release 10 ms).
Notch centres are polled at every block boundary; attenuation is a sine/cosine projection of the last
0.5 s of the left channel. Source: `C:\Users\thorz\AppData\Local\Temp\opencode\fs_probe.cpp`.

| Input | Block / cuts | First LF notch | LF reduction | Plan claim |
| --- | --- | --- | --- | --- |
| 68 Hz, 0.22, in phase | 512 / 8 | 192.0 ms @ 67.93 Hz | 23.98 dB | 192.0 ms, 23.98 dB ✓ |
| Same tone, right channel inverted | 512 / 8 | none in 1 s | 0.00 dB | none, 0 dB ✓ |
| 40 Hz, 0.012, in phase | 64 / 8 | none in 1 s | 0.00 dB | none ✓ |
| Same 40 Hz tone | 1024 / 8 | 213.3 ms @ 39.92 Hz | 23.96 dB | 213.3 ms, 23.96 dB ✓ |
| 68 Hz 0.32 + 1 kHz 0.06 | 512 / **1** | none in 1 s, ends at **999.64 Hz** | 0.00 dB | same ✓ |
| 67.3828125 Hz (exact half bin) | 512 / 8 | 192.0 ms @ 65.68 Hz | 24.24 dB | 24.31 dB ≈ |

One-variable follow-ups isolate the causes:

| Change | Result | Conclusion |
| --- | --- | --- |
| 40 Hz / 64-frame probe with sensitivity −80 (gate forced open) | 192.0 ms, 23.96 dB | the 64-frame miss is **only** the gate |
| Inverted-stereo probe with sensitivity −80 | still none, 0.00 dB | the stereo miss is **only** the mono sum |
| 1-cut mixture with sensitivity −80 | still ends at 999.64 Hz | the mixture miss is **only** the cut budget |

## 2. Where the documents are wrong

### 2.1 "The ±2-bin neighbourhood sits on the peak's own Hann main lobe" — wrong

`core/src/FeedbackSuppressorEffect.cpp:519-520` already uses `prominenceRadius = 2` for the low band
(4 only for the high band) and `kGuardBins = 1`, so the ±1 main-lobe bins are excluded from the
neighbour median. The low band is already the tighter of the two. The proposed frequency-aware radius
and 1/f baseline (P0-3) would loosen high-band rejection without fixing any of the three misses.
Measured: every low-band probe that reaches the FFT is accepted with tonality forced to ≥ 6 dB at the
band maximum (`FeedbackSuppressorEffect.cpp:551-556`).

### 2.2 "240 ms is the time to first cut" — true only for quiet peaks

`confirmPassesForLevel` at `FeedbackSuppressorEffect.cpp:694-700` scales the confirm window by
`clamp(gateAmplitude / peakAmplitude, 0.35, 1.5)`, and the growth rule at `:813-816` removes one more pass
when the level rose ≥ 3 dB. A loud LF tone therefore confirms in 2–3 hops (measured 192.0 ms =
170.7 ms fill + 2 hops), not 6. The 240 ms figure describes a peak hovering at the sensitivity gate, which
is the case the plan is least worried about. The real latency floor at LF is the 170 ms window fill.

### 2.3 "The miss counter resets on every hit and is not a true observation-window ratio" — wrong

`FeedbackSuppressorEffect.cpp:817-820` computes `totalPasses = hits + misses` and requires
`hits / totalPasses >= 0.8`. Misses resetting on a hit does not make the ratio false; it is a rolling
window ratio with a decay that favours recent evidence. This is not a defect.

### 2.4 "The 2 Hz center-accuracy target" — not achievable with 8192 samples and a 3-point fit

At 48 kHz the low band has 5.859 Hz bins; 0.5 bin = 2.93 Hz. Measured on exact half-bin tones
(2 s, 0.3 amplitude, HP 20 Hz), the published centre and the settled cut centre swing by up to
**±2.4 Hz** because the peak bin flips between the two equal bins and the 3-point estimator then reports
+0.5 or −0.5 bin. Measured settled errors: 38.086 Hz → +1.41 Hz; 43.945 Hz → +2.40 Hz; 55.664 Hz → −2.21 Hz;
67.383 Hz → −0.88 Hz; 20.000 Hz → +2.55 Hz.

A 5-point log-magnitude least-squares vertex removes the flip (both peak bins yield the same vertex) and
brings the error to ≤ 1 Hz, at zero extra transform cost. Achieve ≤ 1 Hz at LF and keep the 2 Hz gate only
as a band-wide summary, or zero-pad the LF FFT 2× and pay ~2.2× the transform.

The documents' other accuracy claims do hold: 25 Hz and 30 Hz land within 0.1 Hz, and 20 Hz within
2.55 Hz, so the float32 biquads and the Q clamp at 3 are **not** blockers (P1-4 is unnecessary for the
frequencies this patch supports).

### 2.5 "36 dB cuts below 250 Hz" — contradicts the plan's own acceptance table

The validation table asks for "≥ 12 dB on the three reproduced misses with a 24 dB cap", and all three
misses already reach 23.96–24.31 dB once they detect. `dynamicDepthDb`
(`FeedbackSuppressorEffect.cpp:53-63`) clamps to `kFeedbackSuppressorMaxAttenuationDb` (24 dB), and
`app/src/FeedbackSuppressorWindow.cpp:431` sizes the knob from the same constant. Raising the cap is a UI
and scene-contract change with no reproduced failure behind it. Leave it out.

### 2.6 Period-aware attack `max(attackMs, 2000/f)` capped at 30 ms — internally inconsistent

Two periods at 40 Hz are 50 ms, at 20 Hz 100 ms. A 30 ms cap cannot guarantee two periods below 50 Hz.
Either state the rule as "≥ 1.5 periods, cap 40 ms" or leave the envelope alone in this patch. The
current working-tree defaults (20 ms attack, 300 ms hold, 300 ms release) already remove the sub-period
thump the documents measured against `ed6e3b5`'s 9/10 ms.

### 2.7 Defaults drift: the documents' probes no longer describe this checkout

The uncommitted speech-safe-preset work changes ten defaults
(`core/include/pluginrack/FeedbackSuppressorEffect.h:15-43`): sensitivity −40 dBFS, HP 90 Hz, LP 14 kHz,
attenuation 12 dB, multiplier 1, attack 20 / hold 300 / release 300 ms, confirm 300 ms, 4 cuts.
With HP 90 Hz **no 40 Hz or 68 Hz probe can fire at all**, and with attenuation 12 + multiplier 1 the
quiet 40 Hz probe reaches only ~4 dB. Any probe test must set the detector fields explicitly; the patch
must not touch the defaults.

## 3. What the code already does better than the plan assumes

Do not re-implement these:

- **Level-scaled confirmation** (`:694-700`) and the **growth discount** (`:813-816`) already exist;
  a growth pre-trigger (P0-1) would mostly duplicate the latter.
- **Low-band prominence radius 2 with guard bins** (`:519-520`).
- **In-band duplicate merge** using overlapping −6 dB regions (`:600-654`) and a final separation pass
  (`:900-919`).
- **A replacement path** for a full cut bank (`:877-895`) — it is just too restrictive to ever fire for a
  late LF resonance: it demands `holdSamples <= holdSamples_/2` and `candidate.depthDb > weakest + 6`,
  and depth is capped at 24 dB, so two loud cuts are never 6 dB apart.
- **Normalized `levelDb`** (`20*log10(center / (fftSize*0.25))`, `:582-583`), so ranking detections from
  different FFT sizes needs no extra calibration.

## 4. Confirmed root causes

### 4.1 The gate is per-callback and wipes the ring

`FeedbackSuppressorEffect.cpp:949-971` computes RMS over the current callback only. A 64-frame block at
48 kHz is 1.33 ms; at 40 Hz (25 ms period) a block near a zero crossing is below −43 dBFS, so
`clearAnalysisHistory()` runs and the 8192-sample ring restarts from zero. Confirmed by the forced-gate
experiment above.

Secondary: while the gate is closed, `updateNotches({})` still ages and discards candidates
(`:804-809`), so a single quiet block also erases confirmation progress.

### 4.2 The analysis input is a mono sum

`FeedbackSuppressorEffect.cpp:1016` writes `0.5 * (inL + inR)` into the ring, and the low band FFTs that
single ring. Anti-phase feedback cancels to zero. Confirmed: the inverted probe detects nothing even with
the gate forced open. Note the low-band path already runs one complex radix-2 FFT with `imag = 0`
(`:454-457`), so packing L into the real part and R into the imaginary part yields two channel spectra for
the **same** transform cost. That is cheaper than the documents' "two FFTs".

### 4.3 The high band consumes the shared cut budget, and temporally wins even when ranked second

`:595` computes `maxCuts`, and the peak loop `:600-654` stops as soon as `detections_.size() >= maxCuts`.
The high band is analyzed first with a 2048-sample window that is full after 43 ms; the low band is only
analyzed after 8192 samples (170.7 ms). With one cut the 1 kHz peak is promoted at ~75 ms, and by the time
the bass resonance is detected the bank is full, so `:877-895` refuses to replace it. Cross-band ranking
alone therefore cannot fix the mixture probe — a preemption rule is required.

## 5. Scope decision

**In the patch (four changes, each with a failing test first):**

| Change | Fixes | Risk |
| --- | --- | --- |
| Rolling 50 ms energy gate, −3 dB close hysteresis, 120 ms close dwell, 2 s stale reset; keep-warm ring; freeze candidate aging while closed | 40 Hz / 64-frame miss; gate-flap flapping | low |
| Per-channel detector filtering, two analysis rings, LF magnitude = `max(|L_k|, |R_k|)` via one packed complex FFT; HF input unchanged as `0.5*(L+R)` | inverted-stereo miss | medium (memory ×2, band floor shifts for single-sided content) |
| Per-band shortlist (8 each) merged, sorted by `levelDb`, capped after ranking; preempt the weakest active cut when a fully confirmed candidate is ≥ 6 dB louder | 1-cut mixture miss | medium (a music peak can steal a real cut) |
| 5-point log-magnitude vertex for isolated LF peaks, 3-point/plateau fallback | half-bin center flip, ≤ 1 Hz | low |

**Out of the patch (documented as follow-ups):** 36 dB cap, mid band at 250–1000 Hz, provisional 4096
detector, growth pre-trigger, frequency-aware tonality, double-precision biquads, period-aware envelope,
adaptive detection delay, latched ring-out, RTA overlay, aggregate notches across the chain (P1-8).

P1-8 (`copyActiveNotches` reads only `front()`) is a real, separate bug at
`app/src/AudioEngine.cpp:4798-4820`; fix it in its own commit if desired.

## 6. Adjusted acceptance gates

| Metric | Plan target | Adjusted | Why |
| --- | --- | --- | --- |
| Detection on the three misses | ≥ 12 dB, 24 dB cap | keep | measured 23.96–24.31 dB once detected |
| Time to a cut | ≤ 300 ms | keep | measured 192–213 ms after the gate fix |
| Center error | 95th pct ≤ 2 Hz | **≤ 1 Hz at LF** (5-point fit), ≤ 0.5 bin band-wide | 2 Hz is inside one bin's half-width; 3-point cannot do it |
| False notches | ≤ 1 per 10 min | keep, offline corpus only | not expressible as a unit test; pin the negative cases |
| RT cost | p99 < 50% deadline | measure, no gate | the packed FFT keeps LF transform cost flat; the extra filter chain is ~32 mul/sample |

Negative corpus to pin as tests: 55 Hz + harmonics, pink noise, a kick-like 60 Hz decay, and a 3 dB
crescendo of a 100 Hz tone.

## 7. Risks of the chosen fixes

- **Preemption steals a real HF cut.** Require a fully confirmed candidate (no growth discount),
  `tonalityDb >= 8`, and a 6 dB level margin over the target. Hysteresis is one-way because the new cut is
  louder. Pin with a "slightly louder candidate does not steal the cut" test.
- **Per-channel max shifts levels 6 dB** for content that is only in one channel, changing dynamic depth.
  In-phase stereo is unchanged (mono sum = L = R), which the existing tests cover.
- **Keep-warm can resurrect stale candidates.** Freeze aging while closed, and hard-clear the analysis
  after 2 s below the close threshold.
- **Two rings double detector memory** (~64 KB per instance) and add one more HP/LP chain per sample.
  No allocation in `process()`; both rings are assigned in `prepare()`.
- **The 5-point fit could chase clipped/broad peaks.** Fall back to the existing path unless the peak is
  isolated (outer neighbours ≥ 6 dB down) and the magnitudes are positive.

## 8. Reproduce this review

```powershell
# from the repo root, in a Developer PowerShell (vcvars64) or with absolute cmake:
& $env:ComSpec /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build\verify-core --target pluginrack_tests'
.\build\verify-core\tests\pluginrack_tests.exe "feedback suppressor*"   # 34 cases, 118 assertions, green
```

The probe programs used for the tables are `fs_probe.cpp` (six documented probes, plus a `-80 dBFS`
variant of the gate) and `fs_probe2.cpp` (half-bin accuracy sweep) in
`C:\Users\thorz\AppData\Local\Temp\opencode\`. They build with `cl /std:c++20 /O2 /EHsc` plus
`core/src/FeedbackSuppressorEffect.cpp`. The documents' companion `research/baseline.cpp` does not exist in
this workspace.
