# Spectral Denoiser design

## Goal

Add a built-in **Spectral Denoiser** to PluginRack for reducing steady hiss, fan noise, and room tone beneath speech or music. It is inspired by the workflow of classic spectral denoisers such as Bertom Denoiser Classic, but is an original implementation—not a gate and not a copy of another product's UI or code.

The first version supports two selectable noise-estimation modes:

- **Learn:** capture a noise-only segment, then use its saved profile for reduction.
- **Adaptive:** estimate the noise floor continuously, updating conservatively so wanted audio is less likely to become part of the profile.

Learned profiles are saved with the scene. The effect is available on channel and output strips and follows the strip's ordered effect chain.

## Existing architecture and boundaries

- `FeedbackSuppressorProcessor` in `core/src/FeedbackSuppressorEffect.cpp` contains FFT analysis for detecting feedback, but does not reconstruct audio. Do not reuse its analysis path as a denoiser.
- Built-in strip effects are represented in `Scene`, encoded and validated with the other effects, and processed in ordered chains by both `MixerEngine` and the live `AudioEngine`.
- Follow those existing patterns for a new `SpectralDenoiserEffect` and `SpectralDenoiserProcessor`; keep all FFT buffers and overlap-add state allocated in `prepare()`, never in the render callback.
- Do not alter the existing Dynamics gate or Feedback Suppressor behavior.

## User-facing behavior

- Add **Spectral Denoiser** to channel and output effect menus, chain rows, and the focused built-in effect editor.
- Controls: **Mode** (Learn / Adaptive), **Learn** and **Clear Profile** actions, and **Reduction**. Show learn progress and whether a usable profile exists. If Learn mode has no profile, pass audio through and clearly indicate that learning is needed.
- Show the learned profile as a log-frequency curve in the editor. Split it into 32 bands that can be trimmed ±18 dB and reset individually or together; the graph also draws the trimmed threshold so the effect of a drag is visible. Trims are saved with the effect and applied to both the learned and adaptive noise estimate.
- Learn captures approximately one second of the current input; the user should provide a noise-only segment. A completed profile is stored with the effect and restored with the scene.
- Adaptive mode starts estimating automatically. Use a conservative noise-floor tracker that is slower to learn increases than decreases and is less willing to update during strong tonal/transient content. This mode is a convenience, not a promise that all wanted content will be protected.
- Initialize new instances enabled in Adaptive mode with moderate reduction. At zero reduction, output must be equivalent to the delayed dry signal.
- Bypass is latency-preserving: it outputs dry audio through the same fixed delay as the wet path, so toggling bypass does not shift timing.
- Use the existing editor style and controls. Do not add a dependency or machine-learning model. The profile curve is part of the built-in editor, not a separate analyzer window.

## DSP design

- Use a streaming short-time Fourier transform with a 512-sample frame, 128-sample hop (75% overlap), Hann analysis/synthesis windows, and normalized weighted overlap-add. One uncentered frame of history supplies the fixed delay, so the algorithmic latency is 512 samples (about 10.7 ms at 48 kHz); measure and test the actual implementation rather than assuming the target is met.
- Estimate noise power per frequency bin. Learn mode averages noise-only frame powers into one profile. Adaptive mode maintains a bounded, asymmetric noise-power estimate that avoids quickly following louder rises in the input.
- Compute a Wiener-style gain mask from estimated noise and observed power, scaled by Reduction. Smooth the mask over adjacent frequency bins and over time with separate attack/release behavior to reduce musical-noise artifacts and pumping.
- In stereo, derive one linked gain mask from the combined channel power and apply that same mask to both channels. This prevents image movement and supports mono using the same processor path.
- Keep all frame history, FFT work arrays, profile buffers, and overlap-add queues per processor instance. The audio callback must not allocate, lock, perform file I/O, or serialize scene data.
- The learned profile stores its FFT format, capture sample rate, and per-bin noise power. If the active sample rate differs, remap by frequency with interpolation and conservatively extend the last known level above the captured Nyquist; never reinterpret bin indices as unchanged frequencies.
- Keep algorithmic latency fixed at 512 samples for the 512-point configuration, independent of host block size. Include this built-in latency in live-engine latency reporting/compensation so parallel paths remain aligned. If measured WOLA timing requires a different fixed delay, update the spec and tests before implementation.

## State, learning, and real-time communication

- Add an effect value type with id, bypass, mode, reduction, profile metadata, band trims, and the learned profile bins. Keep the profile owned by the effect/scene state so save/load is deterministic.
- Extend JSON encoding/decoding additively. Older scenes without denoiser fields load with no denoiser instances; new fields use defaults. Do not bump the scene schema version unless compatibility testing shows the reader must interpret an older representation differently.
- Validate mode, finite/ranged controls, profile format/version, sample rate, bin count, finite/non-negative bin values, and the optional band-trim vector (empty or exactly 32 finite values within ±18 dB). Invalid profile data must fail validation rather than enter the audio processor.
- Starting/stopping learning uses bounded atomic commands. The processor captures and publishes completion through a preallocated, lock-free or non-blocking handoff; the message thread applies the finished profile to scene state and handles persistence. No editor callback may mutate DSP vectors concurrently with rendering.
- A changed sample rate rebuilds processor working buffers while retaining/remapping the saved profile as described above. Clearing the profile is an explicit user action.

## Integration

- Add `SpectralDenoiser` as a chain item and add per-strip effect state for both `Channel` and `Output` through their shared `Strip` base where appropriate.
- Integrate processor preparation, ordered processing, live parameter updates, bypass, learn/profile handoff, removal, and scene save/load in both the offline/core mixer and live engine.
- Extend chain serialization, validation, UI menus/cards, and the dedicated effect editor following the existing built-in effect patterns. Existing scenes and chain items must retain their previous meaning and order.
- Include denoiser latency in the live engine's latency accounting and alignment logic; do not report zero latency for a processor that delays audio.

## Validation

- DSP tests: WOLA reconstruction at zero reduction; fixed latency for multiple block sizes; bypass equality to delayed dry; stable shared stereo gain; learned-profile reduction of stationary noise beneath a stronger tone; adaptive convergence without learning a sustained stronger tone as noise; band trims changing how hard a band is cut; finite output and correct reset/sample-rate behavior.
- Real-time safety tests or instrumentation: no allocations or blocking locks during `process()` and no races while requesting/capturing a profile.
- Scene tests: learned-profile, trim, and control round trip, defaults for older scenes, invalid profile/trim rejection, and sample-rate remapping.
- Chain/integration tests: order, bypass, removal, channel/output processing, latency reporting/compensation, and editor updates surviving close/reopen.
- Run focused and full tests, then the required repository build-and-copy script after source/configuration changes so the root `PluginRack.exe` matches the sources.

## Out of scope

- Neural denoising, external model downloads, multiband expanders as a substitute, voice-activity-detection controls, freehand spectral painting beyond the 32-band trims, multiple saved profiles per effect, and standalone plugin formats.
- Exact reproduction of Bertom's controls, appearance, or internal algorithm.
- Removing arbitrary moving noise or guaranteeing transparent results on every source; spectral attenuation trades residual noise against wanted-signal damage and artifacts.
