# PluginRack

A Windows desktop audio mixer, with real VST3 plugin
hosting on every input and an open-ended, many-to-many routing matrix.

PluginRack hosts your own VST3 inserts in the live routing path, so a stream or
recording can run through the plugins you already own instead of a fixed set of
built-in effects.
<img width="1203" height="674" alt="image" src="https://github.com/user-attachments/assets/d6dcdd4e-05f8-4cc1-8030-6cbf7b0b42eb" />

> Status: early development, Windows x64 only. Interfaces and the scene format
> can still change.

> **Plugin trust warning:** plugins render in a separate host process
> (so a crash or hang does not take the app down), but that process
> currently runs with your normal user rights — full OS-level sandboxing
> is not finished yet. Treating a plugin as untrusted is still right:
> never open an untrusted VST3 from the internet or an email, and only
> load plugins from publishers you trust.

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
- **Latency controls** *(BETA)* — an adaptive capture read delay, adaptive
  low-latency input/output FIFOs, a minimal clock FIFO, optional exclusive
  WASAPI, a guided buffer search, a per-stage latency budget with a measured
  round-trip, and per-output monitor/stream classes. Shared WASAPI remains the
  stable fallback.

## Using PluginRack

Launch `PluginRack.exe`. Audio devices open on a background thread, so the window
is usable immediately; the toolbar shows `Starting audio...` with progress while
the driver is probed.

Only one session runs at a time. Launching the app again does not start a second
copy — it brings the running window to the front, restoring it from the tray or
from a minimised state. The headless switches (`--screenshot`, `--list-devices`,
`--list-plugins`, `--scan-plugin`, …) and the out-of-process plugin scanner are
exempt: they are short-lived children of the running app and keep working.

### Views

The toolbar switches between seven views: Racks, Mixer, Patches, I/O,
Routing, Saves, Settings.

- **Racks** — per-channel strips: name, trim/gain, pan, fader and meter, mute/solo,
  the insert list, the docked plugin editor, and routing toggles. Every filled
  insert card carries a read-only thumbnail: the EQ its curve and live spectrum,
  the Dynamics card both transfer curves and their shared gain-reduction bar,
  Multiband Comp and De-esser their spectrum traces, Limiter its ceiling line
  and GR bar, a VST a scaled capture of its editor window. Clicking a
  card puts that insert in the editor bay, which is the only place it can be
  edited. The bay force-fits the plugin's own UI: it is scaled so the tighter of
  width or height exactly fills the bay and the other axis fits, keeping the
  plugin's aspect ratio, so a wide editor such as Ozone's EQ is shrunk to fit
  instead of being cut off. Plugins that do not support host content scaling
  keep their own size and are centred in the bay. The Dynamics card has one
  switch per effect (`C` and `G`); both effects are also switched inside the
  editor panel. Drag a card's six-dot grip to reorder the insert chain; the
  cyan bar shows where it will land.
- **Mixer** — all channel and output strips side by side, with add/remove
  channel/output controls. Each strip's six-dot grip drags it to a new
  position in the row. Double-click a strip to open its Rack page.
- **Patches** — inputs down the left, outputs down the right, with a cable for
  every active send. Click a node to isolate its connections.
- **I/O** — the same strips as the Mixer, split into an **Inputs** tab and an
  **Outputs** tab so a large rack can be patched one side at a time. The strips
  reorder there too, from the same grip. Double-click a strip to open its Rack
  page, like in the Mixer.
- **Routing** — the full channel-to-output matrix with an independent send level
  per cell, L and R linked by default (hold Alt for an independent L/R trim).
- **Saves** — scene save/load and the autosave/last-session entries.
- **Settings** — clock summary, buffers, round-trip measurement, latency tuning,
  device reloads, and the Rack thumbnail refresh rate.

### Channel strips, inserts, and built-in FX

Click an empty insert slot to open the plugin menu. It lists everything found in
the standard VST3 folders as **VST3 ▸ vendor ▸ plugin**, and ends with **Add
plugin folder...** (scan another folder of your own), **Rescan plugin folders**
and **Browse...** (pick a `.vst3` by hand). Left-click a loaded slot to open or
focus its editor, right-click for bypass/removal options. The built-in cards
(EQ, Dynamics, Multiband Comp, De-esser, Limiter) open their own editors; the
Dynamics card opens the panel that carries both compressor and gate, and each
has its own **Enable** switch there. The gate starts bypassed,
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
before the devices reopen. Since scene schema v5 the two self-correcting
switches (**Low-latency input** and **Low-latency outputs**) are on for a new
session; exclusive mode and the minimal clock FIFO stay opt-in. Scenes written
before v5 keep their exact previous behaviour: without the opt-in marker every
switch stays off, so an old save cannot silently select exclusive or
low-latency operation.

- **Find low-latency setting** — steps the buffer down one size at a time,
  listens for real audio events for about 45 s per step, and returns to the size
  you started from on the first glitch. Each step is a normal **Apply**, so a
  device that refuses a size still restores the previous one. The progress line
  shows the step, the size under test and the time left.
- **Input / Output buffer** — `Auto (driver default)` or an explicit size. Auto is
  no longer "whatever the driver felt like": for the output it settles on the
  **smallest buffer the endpoint advertises** and remembers it per device, and
  for the input it requests the clock's own block, so the same rig lands on the
  same numbers on every launch. If a device refuses the new size, the previous
  setting is restored automatically.
- **Latency budget** — the round-trip section lists each stage separately
  (input device, capture read delay, plugin chain, clock FIFO, output device,
  output buffer), then the predicted total, the measured total and the
  **unexplained** remainder. A residual larger than one device block means
  something outside the app is buffering (rate conversion, a virtual cable, a
  driver that runs a larger period than it reports). **Render p99** under it
  shows how much of a block the engine spends, which is the real reason a
  smaller buffer glitches.
- **Low-latency input** *(BETA)* — the capture starts with one render block of
  read delay instead of two (about 10 ms). The read delay is now adaptive: a
  real starvation adds a fraction of a block, and a long calm period gives the
  margin back a few samples at a time.
- **Prefer exclusive mode** *(BETA)* — try exclusive WASAPI for the standalone
  clock output. Shared mode remains first for other streams and same-device
  input because exclusive clients cannot share an endpoint reliably.
- **Low-latency outputs** *(BETA)* — non-clock **monitor** outputs keep two
  device buffers instead of four (about 20 ms on those outputs); bounded drift
  correction keeps separate device clocks from slowly draining that margin.
- **Minimal clock FIFO** *(BETA)* — the clock opens with no primed FIFO. The
  clock also drops its queue to zero once it starts rendering (the block it
  pushes in a callback is the block the device plays in that same callback), so
  this switch only changes how it survives a device hand-off.
- **Same-device input (one-block start)** *(BETA)* — when the input and the clock
  output are the same endpoint, start with one block of read delay.

**Which backend is running:** every strip shows a small tag next to its device
name — `ASIO`, `WASAPI`, `WASAPI LL` (low latency mode) or `WASAPI excl`
(exclusive) — coloured green for ASIO. The Settings clock line repeats it in
brackets, and the latency budget names it on its first row, so the backend is
never a guess.

Latency depends on the driver-reported buffers and plugin chain. Exclusive mode
can be slower or glitch during a switch, so it is only used for the standalone
clock when explicitly requested.

The buffer size in **Auto** is the driver's, not the app's: Windows shared-mode
WASAPI hands out a period of about 10–20 ms, so at 48 kHz that is 480 or 960
frames and at 96 kHz 960 or 1920. PluginRack asks for 48 kHz and `Auto`, so a
960-sample buffer means you are at 20 ms per direction — pick an explicit size
(128, 256) and press **Apply**, and if the driver refuses it the app restores the
previous size and says so. Exclusive WASAPI or ASIO is the only way to go lower.

### ASIO

ASIO is built in whenever JUCE's bundled ASIO headers are present, and the device
menu already lists every ASIO driver next to the Windows Audio endpoints. Pick
the ASIO device name (for example `Focusrite USB ASIO`) as the **clock output**
and the engine drives it directly: the buffer sizes are the interface's own
(typically 64–512), and the driver reports its real input/output latency, which
the latency budget then shows.

**Force ASIO** in Settings does that in one click: it matches the clock's current
Windows endpoint to its ASIO driver by name (`Speakers (Focusrite USB Audio)` →
`Focusrite USB ASIO`), switches the clock to it and arms the duplex switch.
Input strips that name the same interface follow the clock automatically — they
are released from their own client and fed by the clock's callback, so the input
read delay disappears without pressing anything else. This also happens when the
ASIO device is picked from an output's device menu. If the clock is refused, the
inputs are put back exactly as they were.

One rule: an ASIO driver allows a single client — unless you turn on
**Same-device input**. With that switch on, the clock opens the interface for
both directions in one driver callback: capture arrives in the same callback as
the render, so there is no capture ring and no clock FIFO for that interface (the
budget shows a zero clock FIFO and a duplex input figure instead of a read
delay). If the endpoint refuses to open both ways, the app retries output-only
and keeps working.

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

Scenes are validated both ways: `save()` refuses to write a scene the engine
could not restore, and `load()` refuses to apply one. A hand-edited file with
duplicate strip or insert ids, an unknown route target, a chain row naming an
insert the strip does not hold, or a value outside the ranges the controls
expose is rejected with the first concrete reason instead of reaching the mixer.
Loading continues to the next autosave candidate when one file is rejected, so a
damaged `last.json` does not block the session saved next to it.

Because an empty scene is invalid, a channel strip cannot be removed once it is
the last one; the strip reports that instead of disappearing. Exit autosave
failures cannot open a dialog (the process ends immediately afterwards), so the
reason is logged and shown on the next launch.

An insert also records which class inside a multi-class `.vst3` bundle it
hosts, so reopening a scene loads the plugin that was picked rather than the
bundle's first one.

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
| JUCE source tree | With CMake support. Tested against JUCE 9.0.4 (commit 454fe891acc8204f99f4ef1e3a28c84b1a4da145). |
| VST3 SDK | The SDK is only used for hosting; no VST3s are bundled. Tested against VST3 SDK 3.8.1 (commit 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96). |

Fetched dependencies are pinned to exact commit hashes so the build does not
silently change: nlohmann/json 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
(tag v3.11.3) and Catch2 9827c148c397289df17d3967692619a198032a24 (tag
v3.7.1). Other JUCE/VST3 versions may build but are not supported by the
recorded values in the root `CMakeLists.txt`.
| ASIO (optional) | If `JUCE_DIR/modules/juce_audio_devices/native/asio/iasiodrv.h` exists, the build enables ASIO and the device menus expose ASIO devices. |

### Build

The SDK paths are read from the `JUCE_DIR` and `VST3_SDK_DIR` environment
variables when set, or can be supplied on the configure line.

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

This is a JUCE backend characteristic, not a hang. Settings → Latency tuning
now has an opt-in **Cap WASAPI channel probe** toggle and a 2–64 channel value
(default 8). It caps the `queryMaxNumChannels` loop: the two slow USB endpoints
here drop from ~12–13 s to roughly **3 s**, and values 2 and 8 measure the same
because the remaining ~3 s is JUCE's sample-rate probe, which this cap does not
touch. Devices whose default channel count is wider than the cap keep that Windows
default; the cap trims probing above it, never below it. The toggle writes
`PLUGINRACK_WASAPI_MAX_PROBE_CHANNELS`; with it off, JUCE runs its stock probe. The patch lives in
`tools/juce-patches/0001-wasapi-probe-cap.patch` and is applied idempotently at
configure/build time; reverse it with
`git -C "$JUCE_DIR" apply -R tools/juce-patches/0001-wasapi-probe-cap.patch`.

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
- `makeScene()` must not hold `stateLock` across plugin code. It copies the
  strip state under the lock, then asks each insert for its `getStateAsBase64()`
  afterwards: `render()` only yields a few times for that lock before it pushes
  silence, so a slow plugin inside it becomes an audible dropout.
- Input menus are sized from `DeviceState::availableInputs` (via
  `getChannelDeviceState()`), not a fixed count, so interfaces with more than 16
  physical inputs stay patchable.
- **Reset to Defaults must stay complete.** Anything a new strip starts with —
  gains, EQ, dynamics, chain order, routing — has to be restored by the reset
  callback, or the dialog lies about what it did.
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
