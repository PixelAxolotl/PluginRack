# PluginRack

A Windows desktop audio mixer, with real VST3 plugin
hosting on every input and an open-ended, many-to-many routing matrix.

PluginRack hosts your own VST3 inserts in the live routing path, so a stream or
recording can run through the plugins you already own instead of a fixed set of
built-in effects.

> Status: early development, Windows x64 only. Interfaces and the scene format
> can still change.

## What you can do with it

- **Channel strips** with trim, gain, pan, mute/solo, peak metering, and mono or
  stereo layout (mono is upmixed for processing).
- **VST3 inserts** — multiple host slots per channel and per output, hosted in-process,
  with bypass and plugin editors docked in the Rack view.
- **Plugin library** — an empty insert slot opens a menu of what is actually
  installed, grouped as VST3 ▸ vendor ▸ plugin, scanned from the standard VST3
  folders plus any folders you add.
- **Built-in Channel FX** — six-band EQ with delay, plus compressor and gate
  presented as one Compressor / Gate surface.
- **Routing matrix** — every channel can feed every output with an independent
  per-cell send level; outputs can be added and removed while audio is running.
- **Multiple output devices** — each output strip picks its own device. The first
  live output owns the engine clock; every stream shares its sample rate and
  block size.
- **Scenes** — names, gains, pan, mute/solo, EQ, dynamics, insert plugin state,
  routing sends, device choices and the audio tuning are saved as JSON and
  restored exactly.
- **Latency controls** *(BETA)* — device buffer selection, adaptive low-latency
  input/output FIFOs, a minimal clock FIFO, optional exclusive WASAPI, and a
  round-trip latency measurement. Shared WASAPI remains the stable default.

## Using PluginRack

Launch `PluginRack.exe`. Audio devices open on a background thread, so the window
is usable immediately; the toolbar shows `Starting audio...` with progress while
the driver is probed.

### Views

The toolbar switches between six views. `I/O` sits next to `Mixer`.

- **Racks** — per-channel strips: name, trim/gain, pan, fader and meter, mute/solo,
  the insert list, the docked plugin editor, and routing toggles. Every filled
  insert card carries a read-only thumbnail: the EQ its curve and live spectrum,
  the single Gate/Comp card both transfer curves and their shared
  gain-reduction bar, a VST a scaled capture of its editor window. Clicking a
  card puts that insert in the editor bay, which is the only place it can be
  edited. The Gate/Comp card has one switch per effect (`C` and `G`); both
  effects are also switched inside the editor panel.
- **Mixer** — all channel and output strips side by side, with add/remove
  channel/output controls.
- **I/O** — the same strips as the Mixer, split into an **Inputs** tab and an
  **Outputs** tab so a large rack can be patched one side at a time.
- **Routing** — the full channel-to-output matrix with an independent send level
  per cell, L and R.
- **Saves** — scene save/load and the autosave/last-session entries.
- **Settings** — clock summary, buffers, round-trip measurement, latency tuning,
  device reloads, and the Rack thumbnail refresh rate.

### Channel strips, inserts, and built-in FX

Click an empty insert slot to open the plugin menu. It lists everything found in
the standard VST3 folders as **VST3 ▸ vendor ▸ plugin**, and ends with **Add
plugin folder...** (scan another folder of your own), **Rescan plugin folders**
and **Browse...** (pick a `.vst3` by hand). Left-click a loaded slot to open or
focus its editor, right-click for bypass/removal options. The EQ and Gate/Comp
cards are always present; the Gate/Comp card opens the panel that carries both
effects, and each has its own **Enable** switch there. The gate starts bypassed,
so its knobs do nothing until it is switched on. Insert state is saved with the
scene.

### Devices and the clock

Each channel and output strip has a device menu: **System Default**, **None -
silent**, the available WASAPI/ASIO devices, and the app's own device types.
The first output with a real device becomes the clock; its sample rate and render
block drive the engine. Individual devices may use different driver buffer sizes;
the bounded FIFOs bridge those callbacks. Inputs are shared per device, so several
channels can capture the same endpoint.

The **I/O** view shows the same strips as the Mixer with an **Inputs** / **Outputs**
tab switch, so either side can fill the window on its own. Device assignment is
identical in both places.

### Settings

Buffer and latency changes are **staged**: nothing is applied until you press
**Apply**. **Revert** discards the staged values. The tuning travels with the
scene: **Save** stores the buffer sizes, the five latency switches, their
explicit opt-in marker, and the auto-reload state; loading a save restores them
before the devices reopen. All five beta latency switches are off by default.
Scenes written before the opt-in marker are treated as stable defaults, so an
old save cannot silently select exclusive or low-latency operation.

- **Input / Output buffer** — `Auto (driver default)` or an explicit size. If a
  device refuses the new size, the previous setting is restored automatically.
- **Low-latency input** *(BETA)* — the capture starts with one render block of
  read delay instead of two (about 10 ms), then adds a block automatically after
  a real starvation.
- **Prefer exclusive mode** *(BETA)* — try exclusive WASAPI for the standalone
  clock output. Shared mode remains first for other streams and same-device
  input because exclusive clients cannot share an endpoint reliably.
- **Low-latency outputs** *(BETA)* — non-clock outputs keep two device buffers
  instead of four (about 20 ms on those outputs); bounded drift correction keeps
  separate device clocks from slowly draining that margin.
- **Minimal clock FIFO** *(BETA)* — the clock starts with no primed FIFO; a
  missed render is covered by one block of silence (about 10 ms).
- **Same-device input (one-block start)** *(BETA)* — when the input and the clock
  output are the same endpoint, start with one block of read delay. The capture
  adds a block automatically if that aggressive setting starves.

Latency depends on the driver-reported buffers and plugin chain. Start with the
stable shared-WASAPI defaults, then lower the input/output buffers one step at a
time while watching the underrun diagnostics. Exclusive mode can be slower or
glitch during a switch, so it is only used for the standalone clock when
explicitly requested.

### Reloading devices

- **Reload All Audio Devices** reopens every stream with the current settings;
  each device is retried up to three times if it fails, and the previous stream
  stays in place until a new one opens successfully.
- The device list allows **multiple selections**. The checked devices are the
  ones the watchdog watches.
- **Auto-reload checked devices on error** reopens a checked device by itself
  when it starts reporting an error, three rounds five seconds apart. After
  that it stays down until you reload it or the error clears. The switch and the
  checked set are saved with the scene.
- If an Apply or reload cannot open a device, the app keeps or restores the
  previous configuration and reports which device refused it.

### Round-trip latency *(BETA)*

In **Settings**, press **Measure Round-Trip**. The app injects a one-sample
impulse on the clock output and reports how long it takes to come back through
the input chosen in **Listen on** (*Any open input* accepts the first one that
hears it).

This needs a real cable from that output to that input — the app cannot route
the click internally. The result is measured in captured samples, and it is
cleared whenever a device or buffer setting changes because the old figure no
longer describes the open streams.

### Saves

Scenes are stored as JSON under:

```text
%APPDATA%\PluginRack\scenes
```

`Save` writes a new named save; **Saves** lists them with **Load**, **Edit**
(rename), **Delete** and **DEF** (the save opened on startup). The last session
is written on exit.

### The plugin library

The add-plugin menu is filled from:

```text
%APPDATA%\PluginRack\plugins\plugin-library.xml
```

The first launch walks the standard VST3 folders in the background; later
launches reuse the cache and only rescan bundles whose file time changed. Each
bundle is inspected in a short-lived child process, so a plugin that faults
while it is being scanned is recorded in the blocked list instead of taking the
app down. `PluginRack.exe --list-plugins` prints the same catalogue, grouped by
vendor, from the command line.

### Troubleshooting

| Symptom | What to do |
|---|---|
| Toolbar says `Starting audio...` for a long time | Wait for the endpoint probe; the window is usable and the text names the endpoint. A USB device can take ~16 s. Each endpoint is tried once, so it ends as soon as that probe returns. |
| A device is silent or disappeared | Open Settings, tick that device, and press **Reload All Audio Devices**. With **Auto-reload checked devices on error** on, checked devices are reopened (one attempt each). |
| New buffer size is rejected | That device keeps its previous stream and the alert shows the driver's reason. |
| Exclusive mode is refused or slower | Turn **Prefer exclusive mode** off and Apply. Shared WASAPI is the default. |
| Round-trip measurement fails | Connect a physical cable from the clock output to the input chosen in **Listen on**; the app reports "No return" otherwise. |
| The app crashed | The reason is written to `%APPDATA%\PluginRack\logs`: `PluginRack.log` has the run milestones and `crash-<time>.txt` has the fault address and a backtrace. Keep `PluginRack.pdb` next to the exe and run `llvm-symbolizer --obj=PluginRack.exe 0x<address>` to get the function and line. |
| A plugin is refused after a crash | A plugin that crashed the app while it was loading is written to `%APPDATA%\PluginRack\plugins\blocked-plugin-loads.txt` and skipped on later loads, because a fault inside the plugin's own DLL cannot be caught in-process. Delete that line, or run `PluginRack.exe --clear-blocked-plugins`, to load it again. |
| A plugin is suspected of crashing the app | Run `PluginRack.exe --test-plugin "<path to .vst3>"`: it loads that one plugin the same way the Add-VST button does, writes the result to the log and quits, so you can tell a broken plugin from a broken session. |
| A second `PluginRack.exe` appears briefly | Adding a plugin scans its bundle in a short-lived child process (`PluginRack.exe --scan-plugin <file>`). That scan takes seconds for large VST3s and runs outside the window, so the UI keeps drawing and a plugin that faults while scanning cannot take the app down. The child exits as soon as it is done. |
| Quitting does not unload the plugins | `shutdown` ends the process right after the scene is saved. Destroying a VST3 and letting Windows unload its DLL is plugin code, and some of them block or fault there (Native Instruments editors), so PluginRack does not run that teardown. |

## Development

Everything below is for building and working on PluginRack.

### Requirements

| Requirement | Notes |
|---|---|
| Windows 10/11 x64 | The only tested platform today. The architecture keeps macOS/Linux possible. |
| CMake ≥ 3.28 | `CMakePresets.json` uses the Ninja generator. |
| Ninja | Install and make sure `ninja` is on `PATH`, or use another CMake generator. |
| MSVC (C++20) | Visual Studio 2022+ or Build Tools. Run commands from a Developer Command Prompt / after `vcvars64.bat`. |
| JUCE source tree | With CMake support (tested against a local JUCE checkout). |
| VST3 SDK | The SDK is only used for hosting; no VST3s are bundled. |
| ASIO (optional) | If `JUCE_DIR/modules/juce_audio_devices/native/asio/iasiodrv.h` exists, the build enables ASIO and the device menus expose ASIO devices. |

### Build

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
root as `PluginRack.exe` (that copy is git-ignored; the script stops a running
instance if it has to replace the file).

If `JUCE_DIR` or `VST3_SDK_DIR` is missing, CMake still builds the core library
and tests, and prints a warning that the app was skipped.

### Tests

Tests use Catch2 (fetched by CMake on first configure) and cover the core engine,
scene codec/validation, metering, EQ, dynamics, input mapping, offline rendering,
and source-level invariants of the audio path.

```powershell
ctest --test-dir build/dev --output-on-failure
# or
cmake --build build/dev --target pluginrack_tests
build/dev/tests/pluginrack_tests.exe
```

The first CMake configure is slow because Catch2 is fetched from GitHub; JUCE
and the VST3 SDK are configured from the given paths.

### Project layout

```text
app/     JUCE GUI application
  src/   MainComponent views, AudioEngine, DeviceStream (WASAPI/ASIO),
         PluginHost (VST3), PluginLibrary/PluginMenu (installed-plugin catalogue
         and the add-plugin menu), strips, EQ/dynamics windows, I/O and Settings views
core/    Platform-independent C++20 audio/routing core (no JUCE)
  include/pluginrack/  Scene model, mixer engine, EQ, dynamics, meters,
                       offline renderer, WAV writer, validation
  src/
tests/   Catch2 tests for the core and source-level audio-path invariants
tools/   render_cli and the build-and-copy helper
docs/    Design spec and session handoff notes
```

### Why the first device open can take a while

Opening a WASAPI endpoint is not instant. JUCE's WASAPI backend probes every
channel count (up to 64 down to 2 in the current JUCE source) across every supported sample rate and format
(`queryMaxNumChannels`) before it can report the device's capabilities, and some
USB drivers answer each format check slowly.

On the development machine two USB endpoints take about **16 seconds each** in
that probe; other devices take well under a second. Independent outputs and
captures are opened concurrently, so startup is bounded by roughly the slowest
single endpoint rather than the sum of all probes. The toolbar names the
endpoint being probed (`Starting audio... 2/3  input "Ch 1" - USB Audio`), so a
slow driver is visible instead of looking like a hang.

Each endpoint is attempted **exactly once**. There is no device-type fallback
chain, no default-rate retry, and no second pass: if an endpoint refuses, its
stream stays closed and silent, the strip goes red, and the message names the
strip, the endpoint and the driver error. The first output is the clock, so if
it cannot be opened nothing can play and the message says so. Exclusive mode
adds its more expensive format probe and is only used when explicitly enabled.
Reloading a single device avoids paying that cost for every stream.

This is a JUCE backend characteristic, not a hang. If it becomes a problem, the
options are a local JUCE patch that caps the channel-count probe, caching the
working device mode per endpoint, or using ASIO where a driver exists.

### Development notes

- The audio thread must not allocate or block. Meter/analyzer paths read from
  lock-free taps; structural changes take `AudioEngine::stateLock`.
- The UI must never wait on the audio thread. Device opens run on the background
  opener (`openStripsDevicesAsync` / `applyDeviceSettings`) and are guarded by
  `openingDevices_`; structural mutators must respect that guard.
- **Never read `inputDeviceName` / `outputDeviceName` / `inputError` /
  `outputError` from a view.** The opener assigns those `juce::String`s on its
  worker thread; copying one while it is being reassigned reads a torn
  `StringHolder` pointer and crashes inside `String::operator=`. Views use the
  locked snapshots `getChannelDeviceState()`, `getOutputDeviceState()`,
  `getDeviceErrors()` and `getAssignedInputDeviceNames()`.
- Device streams are replaced only after a successful open. A failed open keeps
  the previous playback/capture, which is what makes Apply and device reloads
  safe to try.
- MSVC builds use `/W4 /WX`; keep new code warning-clean. Every build also emits
  `PluginRack.pdb` next to the exe so a crash offset can be symbolized.
- Add core behavior tests next to the existing Catch2 cases. The source-invariant
  tests in `tests/test_underrun_paths.cpp` protect the audio-path wiring.
- Scenes are schema-versioned. Bump `kSchemaVersion` and read new fields with
  defaults; the audio tuning block is `std::optional` so older saves keep the
  live settings.
- One type, spacing and control vocabulary lives in `theme::ui` (`app/src/Theme.h`);
  page views share `PageHeader`, buttons come from `ConsoleLookAndFeel`, and new
  surfaces should use those instead of local colours and fonts.

### Documentation

- [PRODUCT.md](PRODUCT.md) — product definition, users, and constraints.
- [docs/superpowers/specs/2026-10-28-pluginrack-design.md](docs/superpowers/specs/2026-10-28-pluginrack-design.md) — approved design.
- [docs/NEXT_SESSION_MIXER.md](docs/NEXT_SESSION_MIXER.md) — build commands and handoff notes.

## License

No license file is included in this repository. All rights are reserved by the
repository owner unless a license is added.
