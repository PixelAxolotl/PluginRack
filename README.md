# PluginRack

A Windows desktop audio mixer in the spirit of Voicemeeter, with real VST3 plugin
hosting on every input and an open-ended, many-to-many routing matrix.

PluginRack hosts your own VST3 inserts in the live routing path, so a stream or
recording can run through the plugins you already own instead of a fixed set of
built-in effects.

> Status: early development, Windows x64 only. Interfaces and the scene format
> can still change.

## Features

- **Channel strips** with trim, gain, pan, mute/solo, peak metering, and mono or
  stereo layout (mono is upmixed for processing).
- **VST3 inserts** — multiple host slots per channel and per output, hosted in-process,
  with bypass and plugin editors docked in the Rack view.
- **Built-in Channel FX** — six-band EQ with delay, plus compressor and gate
  presented as one Compressor / Gate surface.
- **Routing matrix** — every channel can feed every output with an independent
  per-cell send level; outputs can be added and removed while audio is running.
- **Multiple output devices** — each output strip picks its own device. The first
  live output owns the engine clock; every stream shares its sample rate and
  block size.
- **Scenes** — names, gains, pan, mute/solo, EQ, dynamics, insert plugin state,
  routing sends and device choices are saved as JSON and restored exactly.
- **Latency controls** — device buffer selection, optional low-latency input,
  low-latency output FIFOs, a minimal clock FIFO, exclusive WASAPI, and a
  round-trip latency measurement.

## Requirements

| Requirement | Notes |
|---|---|
| Windows 10/11 x64 | The only tested platform today. The architecture keeps macOS/Linux possible. |
| CMake ≥ 3.28 | `CMakePresets.json` uses the Ninja generator. |
| Ninja | Install and make sure `ninja` is on `PATH`, or use another CMake generator. |
| MSVC (C++20) | Visual Studio 2022+ or Build Tools. Run commands from a Developer Command Prompt / after `vcvars64.bat`. |
| JUCE source tree | With CMake support (tested against a local JUCE checkout). |
| VST3 SDK | The SDK is only used for hosting; no VST3s are bundled. |
| ASIO (optional) | If `JUCE_DIR/modules/juce_audio_devices/native/asio/iasiodrv.h` exists, the build enables ASIO and the device menus expose ASIO devices. |

## Build

The SDK paths default to the author's machine (`C:/Users/thorz/JUCE` and
`C:/Users/thorz/vst3sdk`). Override them on the configure line.

```powershell
# From a Visual Studio Developer PowerShell / after vcvars64.bat
cmake -B build/dev -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DJUCE_DIR="C:/path/to/JUCE" `
  -DVST3_SDK_DIR="C:/path/to/vst3sdk"

cmake --build build/dev --target pluginrack_app
```

Or use the checked-in preset for a Debug build:

```powershell
cmake --preset dev
cmake --build --preset dev --target pluginrack_app
```

The executable is written to:

```text
build/dev/app/pluginrack_app_artefacts/Release/PluginRack.exe   # or Debug/
```

`tools/build-and-copy.ps1` rebuilds and copies the executable to the repository
root as `PluginRack.exe` (that copy is git-ignored).

If `JUCE_DIR` or `VST3_SDK_DIR` is missing, CMake still builds the core library
and tests, and prints a warning that the app was skipped.

## Tests

Tests use Catch2 (fetched by CMake on first configure) and cover the core engine,
scene codec/validation, metering, EQ, dynamics, input mapping, offline rendering,
and source-level invariants of the audio path.

```powershell
ctest --test-dir build/dev --output-on-failure
# or
cmake --build build/dev --target pluginrack_tests
build/dev/tests/pluginrack_tests.exe
```

## Running

Launch `PluginRack.exe` from the build output or the repository root. Audio
devices open on a background thread, so the window is usable immediately; the
toolbar shows `Starting audio...` with progress while the driver is probed.

Scenes are stored as JSON under:

```text
%APPDATA%\PluginRack\scenes
```

## Using PluginRack

### Views

- **Racks** — per-channel strips: name, trim/gain, pan, fader and meter, mute/solo,
  the EQ slot, the VST insert list, the docked plugin editor, and routing toggles.
- **Mixer** — all channel and output strips side by side, with add/remove
  channel/output controls.
- **Routing** — the full channel-to-output matrix with a send level per cell.
- **Saves** — scene save/load and the autosave/last-session entries.
- **Settings** — clock summary, latency, buffers, latency tuning, device reloads,
  and round-trip measurement.

### Inserts and Channel FX

Click an empty insert slot to load a VST3. Left-click a loaded slot to open or
focus its editor, right-click for bypass/removal options. The EQ slot is always
present; the Compressor / Gate editor opens from the strip as well. Insert state
is saved with the scene.

### Devices and the clock

Each channel and output strip has a device menu: **System Default**, **None -
silent**, the available WASAPI/ASIO devices, and the app's own device types.
The first output with a real device becomes the clock; its sample rate and block
size are used by every stream. Inputs are shared per device, so several channels
can capture the same endpoint.

### Settings

Buffer and latency changes are **staged**: nothing is applied until you press
**Apply**. **Revert** discards the staged values. These settings are session-only
and return to their defaults when the app restarts; scenes persist the audio
setup (devices, plugins, routing), not the tuning options.

- **Input / Output buffer** — `Auto (driver default)` or an explicit size. If a
  device refuses the new size, the previous setting is restored automatically.
- **Low-latency input** — the capture keeps one render block of read delay
  instead of two (about 10 ms).
- **Prefer exclusive mode** — try exclusive WASAPI before the shared modes. It can
  help or hurt depending on the driver; switching reopens every device.
- **Low-latency outputs** — non-clock outputs keep two device buffers instead of
  four (about 20 ms on those outputs).
- **Minimal clock FIFO** — the clock starts with no primed FIFO; a missed render
  is covered by one block of silence (about 10 ms).
- **Same-device input (full duplex)** — when the input and the clock output are
  the same endpoint, use its hardware clock and a one-block read delay.

On the development machine (48 kHz, 480-sample blocks, USB clock output) the
defaults measured about **70 ms** end-to-end estimate, **60 ms** with low-latency
input, and **50 ms** with the minimal clock FIFO as well. Exclusive mode measured
higher here (~80–90 ms) and glitched once while switching; that is driver-dependent.

### Reloading devices

- **Reload All Audio Devices** reopens every stream with the current settings.
- The device list allows **multiple selections**. Tick the devices you want and
  press **Reload Selected Devices (N)**. Each device is retried up to three times
  if it fails, and the previous stream stays in place until a new one opens
  successfully.
- If an Apply or reload cannot open a device, the app keeps or restores the
  previous configuration and reports which device refused it.

### Round-trip measurement

Connect an output to an input (loopback), then press **Measure Round-Trip
Latency**. The app injects a one-sample impulse and reports the measured latency
in milliseconds.

## Why the first device open can take a while

Opening a WASAPI endpoint is not instant. JUCE's WASAPI backend probes every
channel count from 32 down to 2 across every supported sample rate and format
(`queryMaxNumChannels`) before it can report the device's capabilities, and some
USB drivers answer each format check slowly.

On the development machine two USB endpoints take about **16 seconds each** in
that probe; other devices take well under a second. Outputs are therefore opened
concurrently, so the wall-clock startup is roughly the slowest single endpoint
rather than the sum. The UI stays responsive and shows progress. Reloading a
single selected device avoids paying that cost for every stream.

This is a JUCE backend characteristic, not a hang. If it becomes a problem, the
options are a local JUCE patch that caps the channel-count probe, caching the
working device mode per endpoint, or using ASIO where a driver exists.

## Project layout

```text
app/     JUCE GUI application
  src/   MainComponent views, AudioEngine, DeviceStream (WASAPI/ASIO),
         PluginHost (VST3), strips, EQ/dynamics windows, SettingsView
core/    Platform-independent C++20 audio/routing core (no JUCE)
  include/pluginrack/  Scene model, mixer engine, EQ, dynamics, meters,
                       offline renderer, WAV writer, validation
  src/
tests/   Catch2 tests for the core and source-level audio-path invariants
tools/   render_cli and the build-and-copy helper
docs/    Design spec and session handoff notes
```

## Development notes

- The audio thread must not allocate or block. Meter/analyzer paths read from
  lock-free taps; structural changes take `AudioEngine::stateLock`.
- The UI must never wait on the audio thread. Device opens run on the background
  opener (`openStripsDevicesAsync` / `applyDeviceSettings`) and are guarded by
  `openingDevices_`; structural mutators must respect that guard.
- Device streams are replaced only after a successful open. A failed open keeps
  the previous playback/capture, which is what makes Apply and device reloads
  safe to try.
- MSVC builds use `/W4 /WX`; keep new code warning-clean.
- Add core behavior tests next to the existing Catch2 cases. The source-invariant
  tests in `tests/test_underrun_paths.cpp` protect the audio-path wiring.

## Documentation

- [PRODUCT.md](PRODUCT.md) — product definition, users, and constraints.
- [docs/superpowers/specs/2026-10-28-pluginrack-design.md](docs/superpowers/specs/2026-10-28-pluginrack-design.md) — approved design.
- [docs/NEXT_SESSION_MIXER.md](docs/NEXT_SESSION_MIXER.md) — build commands and handoff notes.

## Troubleshooting

| Symptom | What to do |
|---|---|
| Toolbar says `Starting audio...` for a long time | Wait for the endpoint probe; the window is usable. A USB device can take ~16 s. |
| A device is silent or disappeared | Open Settings, tick that device, press **Reload Selected Devices**. Failed devices are retried automatically. |
| New buffer size is rejected | The app restores the previous size and shows the driver's reason in an alert. |
| Exclusive mode is refused or slower | Turn **Prefer exclusive mode** off and Apply. Shared low-latency is the default. |
| Round-trip measurement fails | Connect a physical loopback from the output to the input; the app reports "no loopback detected" otherwise. |
| First CMake configure is slow | Catch2 is fetched from GitHub; JUCE and the VST3 SDK are configured from the given paths. |

## License

No license file is included in this repository. All rights are reserved by the
repository owner unless a license is added.

