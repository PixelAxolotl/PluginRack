# Opt-in cap for JUCE's WASAPI channel probe

| Field | Value |
| --- | --- |
| Date | 2026-11-13 |
| Status | Design — approved, awaiting spec review |
| Stack | Windows 10/11 x64 · C++20 · MSVC `/W4 /WX` · CMake ≥ 3.28 + Ninja · JUCE 9.0.4 (commit `454fe891`) |
| Build gate | `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`; `ctest --test-dir build/dev --output-on-failure` |

## 1. Problem

The first audio-device open can take 12–16 s per slow WASAPI endpoint on the
development machine. The cost is entirely inside JUCE:

- `juce_WASAPI_windows.cpp:831` `queryMaxNumChannels` loops channel counts from
  `AudioChannelSet::maxChannelsOfNamedLayout` (64) down to the endpoint's
  default (2 for the two USB endpoints here).
- For every channel count it tries every supported sample rate × 7 sample
  formats, calling `tryFormat` / `IsFormatSupported` each time. That is roughly
  1,700 driver round-trips.
- Slow USB drivers answer each check in ~7 ms, which is the 12–13 s.

Measured on 13 Nov 2026 (see `.superpowers/sdd/plan/progress.md`): device-open
window 12.60 s cold, 13.28 s warm; `Speakers (USB PnP Audio Device)` probe
12,415 ms, `Sub (YA USB Audio)` 12,292 ms, while M-Audio endpoints take
~0.7–1.0 s. The persisted device cache cannot skip this probe.

JUCE's `AudioIODeviceType::createDevice` only receives device names, so the
backend cannot know how many channels the app will request. A cap must be a
value we choose and pass into the backend.

## 2. Goals and non-goals

**Goals**

- An opt-in setting, **off by default**, that caps the channel-count probe.
- A numeric cap the user chooses (2–64, default 8), not only a boolean.
- Off means stock JUCE behavior, with no measurable difference.
- The value persists with the scene and survives restarts.
- Applying the setting reopens devices so the effect is testable immediately.
- Failure to patch JUCE is visible and non-fatal; the app still runs.

**Non-goals**

- Skipping `querySupportedSampleRates`, caching probe results, or opening
  WASAPI directly. Those are separate changes.
- Per-endpoint caps. The cap applies to every WASAPI endpoint in the process.
- Changing the global default or enabling the cap automatically on slow devices.
- CI changes. CI builds with `PLUGINRACK_BUILD_APP=OFF`, so it never compiles
  JUCE and is unaffected.

## 3. Design

### 3.1 The JUCE patch

New file: `tools/juce-patches/0001-wasapi-probe-cap.patch`.

It changes one function only, `WASAPIDeviceBase::queryMaxNumChannels`:

```cpp
     int queryMaxNumChannels (IAudioClient* clientToUse) const
     {
         static constexpr auto maxNumChannelsToQuery =
             static_cast<int> (AudioChannelSet::maxChannelsOfNamedLayout);
         const auto fallbackNumChannels = defaultNumChannels;

         auto highestChannelToQuery = maxNumChannelsToQuery;

         // PluginRack: opt-in cap (absent/0/invalid = stock behaviour). The app
         // sets this from Settings; it skips the 64..2 channel probe on drivers
         // that answer each format check slowly.
         if (const auto cap = SystemStats::getEnvironmentVariable (
                 "PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS", {}).getIntValue();
             cap >= 2)
             highestChannelToQuery = jmin (highestChannelToQuery, cap);

         if (fallbackNumChannels >= highestChannelToQuery)
             return fallbackNumChannels;

         auto result = fallbackNumChannels;

         for (auto ch = highestChannelToQuery; ch > result; --ch)
         ...
```

The patch is env-driven and defaults to stock, so a patched JUCE checkout
behaves exactly as before for every other project that uses it.

### 3.2 Patch application

The JUCE source tree is external (`JUCE_DIR`, normally a git checkout) and is
not vendored in this repo. PluginRack applies the patch idempotently, never
silently:

- New CMake script: `tools/juce-patches/apply-wasapi-probe-cap.cmake`, run in
  script mode (`cmake -P`) with `-DJUCE_DIR=...` and `-DPATCH_FILE=...`.
  Logic:
  1. If `JUCE_DIR/.git` is missing, print a status and stop (nothing to patch).
  2. `git -C JUCE_DIR apply --check --reverse PATCH` succeeds → already applied.
  3. Else `git -C JUCE_DIR apply --check PATCH` succeeds → apply it and print
     what happened.
  4. Else print a `WARNING` naming the patch and saying the toggle will have no
     effect until the patch applies.
- Top-level `CMakeLists.txt` runs the script at configure time when
  `PLUGINRACK_BUILD_APP` is on, guarded by a new cache option
  `PLUGINRACK_PATCH_JUCE_WASAPI_PROBE` (default `ON`).
- `tools/build-and-copy.ps1` runs the same script before `cmake --build`
  (reading `JUCE_DIR` from the CMake cache). This catches a JUCE checkout that
  was reset or updated after the build directory was configured.
- Reversing is documented: `git -C "$JUCE_DIR" apply -R <patch>`.

Git is already required (Catch2 is fetched with FetchContent), so no new
dependency.

### 3.3 Runtime channel

Environment variable: `PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS`.

- `0`, absent, or unparsable → no cap (stock JUCE).
- `2..64` → the probe starts at `min(64, value)`.
- The engine constructor forces the variable to `0` so an inherited environment
  value cannot silently cap a launch.
- `AudioEngine::applySettingsValues` writes the value with `_putenv_s(name,
  String(value))` before any device job starts, guarded by `#if JUCE_WINDOWS`.
  This runs for scene restore (before the startup open) and for every Apply.

### 3.4 Settings, core rule and scene

New core header/source pair `core/include/pluginrack/AudioDeviceProbe.h`,
`core/src/AudioDeviceProbe.cpp`:

```cpp
namespace pluginrack {
inline constexpr int kWasapiProbeCapMinChannels = 2;
inline constexpr int kWasapiProbeCapMaxChannels = 64;
// 0 = no cap (stock JUCE). Enabled values are clamped to [2, 64].
int wasapiProbeCapValue(bool enabled, int channels);
}
```

`AudioEngine::DeviceSettings` gains:

```cpp
bool wasapiProbeCapEnabled = false;
int wasapiProbeCapChannels = 8;
```

Scene schema rises from 6 to 7:

- `AudioSettings` gains the two fields; `SceneCodec` writes them in the audio
  object and reads them with defaults `false` / `8`.
- `schemaVersion < 7` forces `wasapiProbeCapEnabled = false` and
  `wasapiProbeCapChannels = 8`, matching the v6 gating pattern.
- `SceneCodec` accepts `1..7`; `SceneValidator` does too and reports an error
  for an enabled cap outside `2..64`. The codec clamps defensively on read so a
  hand-edited file cannot wedge the engine.
- Scene save/restore maps the fields both ways in `AudioEngine` next to the
  existing latency options.

### 3.5 UI

In the existing "Latency tuning (opt-in)" section of `SettingsView`:

- A toggle labelled **"Cap WASAPI channel probe"**, off by default.
- An inc/dec numeric field (2–64, suffix ` ch`, default 8), enabled only while
  the toggle is on.
- Tooltip: faster startup; devices with more I/O than the cap report the cap
  while it is enabled; off leaves JUCE untouched.
- Both controls are staged like the other tuning switches, participate in the
  "settings changed" check that enables Apply, and refresh from the applied
  settings.

### 3.6 Behavior

- Off: env var `0`, JUCE runs the stock 64→2 probe. No behavior change.
- On at N: the probe starts at N. Measured on 13 Nov 2026 with the two slow USB
  endpoints: ~12.2 s with the toggle off, ~3.1 s at N=8, ~3.0 s at N=2. The
  channel loop is gone at either value; the remaining ~3 s is
  `querySupportedSampleRates`, which this change deliberately leaves alone.
- A device whose real channel count exceeds N reports and opens at most N
  channels while the cap is on. This is the documented trade of the opt-in.
- Apply persists the value, updates the env var, and reopens devices through the
  existing `applySettingsValues` → `openStripsDevicesNow` path.
- The next launch restores the scene value before the startup open.

## 4. Testing

Core (new `tests/test_audio_device_probe.cpp`):

- `wasapiProbeCapValue(false, 8) == 0`
- enabled: `2→2`, `8→8`, `64→64`, `0→2`, `1→2`, `65→64`

Scene (`tests/test_scene_codec.cpp`, `tests/test_scene_validator.cpp`):

- v7 round-trip of both fields.
- loading a v6 scene forces `enabled=false`, `channels=8`.
- validator accepts 2..64 and rejects 1/65 when enabled.

Source-invariant scans (`tests/test_underrun_paths.cpp`), matching the repo's
pattern for app code the core test binary cannot link:

- the patch file exists and contains `PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS`
  and `highestChannelToQuery`;
- top-level CMake and the build script reference the apply script;
- `applySettingsValues` writes the environment variable;
- `SettingsView` has the toggle and numeric control;
- `SceneCodec` reads and writes both fields.

Build and manual acceptance:

1. Configure/build: the patch applies, `PluginRack.exe` is rebuilt and copied.
2. Toggle off, relaunch: `device: open ... probe_ms=` stays ~12–13 s for the USB
   endpoints.
3. Enable with 8, Apply, relaunch: the same endpoints log roughly 3 s.
4. Set 2, Apply, relaunch: roughly the same ~3 s; the remaining cost is the
   sample-rate probe, not the channel loop.
5. Toggle off, Apply, relaunch: ~12–13 s returns.
6. `git -C "$JUCE_DIR" status` shows only the patched file modified.

## 5. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Patching a user-owned SDK is surprising or destructive | One-function patch, idempotent check, documented reverse, clear CMake message/reverse command |
| JUCE checkout updated or reset; patch no longer applies | Warning at configure and at build; the toggle becomes inert, the app still runs |
| User caps below a device's real channel count and loses inputs | Opt-in, numeric control shows the value, tooltip states the trade; turning it off restores full capability on the next open |
| Inherited environment value caps a launch unexpectedly | Engine constructor forces `0`; only Settings writes a non-zero value |
| Env var read on the audio thread | No: the probe runs on the device-opener thread during `initialise` |
| Other projects share the patched JUCE checkout | Patch is inert unless the env var is set; it is only set inside PluginRack |

## 6. Out of scope

- Caching or otherwise skipping `querySupportedSampleRates`.
- Per-endpoint caps or auto-tuning the cap from the device cache.
- A direct `IAudioClient` clock open.
- UI "probing" state or progress estimates.
- Changing the JUCE default for everyone (the patch stays opt-in and env-gated).
