# PluginRack — Handoff Document for Next Session/Agent

## Fault containment (2026-11-13) — PARKED, diagnostics only

Read first: `docs/superpowers/plans/2026-11-13-plugin-fault-containment.md`
§6.0 and `docs/superpowers/specs/2026-11-13-fault-containment-ipc.md` §7.1.

- Branch `feat/audio-robustness` holds the whole attempt; `master` has none of
  it (Fix 1 `5132a41` is already on master and must stay green).
- Working today: `--test-child-host <vst3> [--kill-after-ms N]` renders one
  plugin in a child process and proves the insert goes dry when the child is
  killed; `--test-child-null <vst3>` proves child output is bit-identical to
  in-process output (with a determinism baseline). Seven plugins identical at
  48 kHz / 256.
- Not wired: no insert in the app uses a child. The engine wiring was written,
  crashed 4 of 8 twenty-second soaks under live device audio, and was reverted.
- Resume gate: the four fixes in plan §6.0 (explicit renderer mode, safe host
  lifetime, no prepare/control on the render path, serialised control calls).
  Do not re-wire before those, and accept the result only on a 30-run soak with
  zero crashes.
- `PluginHost::setChildParameter()` is deliberately caller-less (Phase 2 entry).

## Project Overview

PluginRack is a Windows desktop audio mixer (JUCE/VST3) with per-channel and per-output VST3 insert racks, multichannel input support, and latency diagnostics. Built with C++20, JUCE 9, MSVC `/W4 /WX`.

## Current State (as of 2026-10-29)

### Git
- Branch: `master`
- Latest commit: `adf0d65 feat: app shell with toolbar and view navigation`
- Pushed to origin: yes
- Working tree: clean (only untracked plan docs + specs)
- `PluginRack.exe` copied to repo root (gitignored)

### Build & Tests
- Build: `cmake --build --preset dev --target pluginrack_app` — clean under `/W4 /WX`
- Tests: `ctest --preset dev` — **57/57 pass**
- Build dir: `build/dev/`
- Compiler: MSVC via `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat`

### Plans Completed

| Plan | Tasks Done | Commit Range |
|------|-----------|--------------|
| Output FX | 5/5 | `bd05d86` → `da437a4` |
| Split Multichannel Inputs | 5/6 | (docs deferred) |
| Latency Reduction | 6/7 | (validation = build/tests pass) |
| Rack UI | 1/7 | `adf0d65` (app shell + toolbar + view nav) |

### Key Architecture

- **Core library** (`core/`): JUCE-free, nlohmann_json. `Scene`, `SceneCodec`, `SceneValidator`, `MixerEngine`, `InsertChain`.
- **App** (`app/src/`): JUCE desktop app. `AudioEngine`, `DeviceStream`, `MainComponent`, `MixerComponent`, `ChannelStrip`, `InsertHost`, `PluginHost`.
- **Tests** (`tests/`): Catch2. 57 tests. `test_input_mapping.cpp` added for split-inputs.

### Key Files Modified (recent)

- `app/src/AudioEngine.h/.cpp`: `prepareToPlay(int)`, `setDeviceBufferSize()`, `openFullDuplexIfPossible()`, `getFifoDiagnostics()`, `getHighestLatencyInsert()`, `setChannelName/Layout/InputChannels`, shared input captures
- `app/src/DeviceStream.h/.cpp`: multichannel capture, `pull(dest, frames, physicalChannels)`, bounded FIFOs, overflow/underflow atomics
- `app/src/MainComponent.h/.cpp`: `ToolbarComponent`, `ViewRouter`, `AppView` enum (Rack/Mixer/Routing/Settings), placeholder views
- `app/src/InsertHost.h`: `IInsertHost` interface
- `core/src/Scene.h`: `Output::inserts`, `Channel::inputChannels`

### Pending Work

See `docs/superpowers/plans/2026-10-29-pending-tasks.md` for full list. Summary:

1. **Rack UI Tasks 2–7**: Rack view with horizontal plugin modules, chain selection, dedicated mixer view, routing view (instantiate `RoutingMatrixComponent`), settings view, meters/states, legacy removal. Task 1 (app shell) done. Placeholder views in `ViewRouter` need to be replaced with real components.

2. **Make channels renamable**: `AudioEngine::setChannelName()` already exists. Need UI affordance in `ChannelStrip` (double-click name label or dialog). Persist via existing scene codec.

3. **Split inputs Task 6**: Update `ui.md` with physical input assignment docs.

4. **Latency Task 7 (formal)**: Already satisfied (build clean + tests pass + exe shipped).

#### Audio stability (2026-10-31)

- `InputCapture` is lock-free on the writer side and reads through
  `pluginrack::CaptureReadCursor` (`core/include/pluginrack/CaptureCursor.h`):
  a fixed two-device-buffer delay with drift-corrected fractional reads, so
  input/output callback phase jitter can no longer repeat or skip a whole
  block (the old "read the newest frames" behaviour). New counters:
  `getInputUnderruns()`, `getInputOverruns()`, `getInputDroppedFrames()`.
- `OutputPlayback` keeps a four-buffer target fill, counts low-watermark
  **events** (not every callback), fades short reads to silence and ramps
  back in, and `pushSilence()` tops the FIFO back up on a render miss.
- `AudioEngine::render` uses a bounded `BriefTryLock`: short UI critical
  sections become a small render delay instead of a dropped block. On a real
  miss it feeds faded silence from a lock-free published playback snapshot
  (`renderOutputs_`, with retirements kept alive in `retiredOutputs_`).
- Plugin `prepareToPlay`/`releaseResources` now run outside `stateLock`, so a
  device reopen or scene restore no longer starves every output.
- Tests: `tests/test_capture_cursor.cpp` (real cursor unit tests) plus the
  updated static invariants in `tests/test_underrun_paths.cpp`.

## How to Build

```powershell
# Set up MSVC environment
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" >nul 2>&1 && cmake --build --preset dev --target pluginrack_app"
# Run tests
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" >nul 2>&1 && ctest --preset dev"
```

### How to Continue

1. **Rack UI Tasks 2–7**: Start with Task 2 (RackView). The `ViewRouter` currently uses `PlaceholderView` for Rack/Routing/Settings. Replace with real components. `MixerComponent` already exists and can be reused for the Mixer view. `RoutingMatrixComponent` exists (currently dead code) — instantiate in Routing view. Create `SettingsView` for device/buffer/latency diagnostics.

2. **Make channels renamable**: Add rename affordance to `ChannelStrip`. `AudioEngine::setChannelName()` is ready.

3. **Ship**: After all work, rebuild, test, copy exe to repo root, push.

### Important Notes

- The `ViewRouter` and `ToolbarComponent` are inline in `MainComponent.cpp/.h` (not separate files).
- `RackView.h/.cpp` was created but not wired in (include removed). Can be used as starting point.
- `RoutingMatrixComponent` exists but is dead code — needs instantiation in Routing view.
- Concurrent workers may be editing the same files. Check `git status` before starting.
- PluginRack.exe is gitignored. Copy manually after build.
- No `ui.md` file exists yet (referenced in split-inputs plan Task 6).