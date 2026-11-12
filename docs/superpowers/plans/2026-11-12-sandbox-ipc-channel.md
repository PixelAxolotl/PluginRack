# Plan: Fix "app suddenly closed" / sandboxed host IPC failure

> Preserved from the repository-root `plan.md` before it was replaced by the
> 2026-11-12 audio-stutter investigation. Work described here is complete.

## Problem (from logs)

Today's run log (`%APPDATA%\PluginRack\logs\PluginRack.log`) shows no crash report, but every
plugin's restricted host child starts and immediately dies:

```
plugin: restricted host started for Ozone Imager 2.vst3
plugin: restricted host lost for Ozone Imager 2 (exit 0x1) HOSTREADY ...
HOSTERROR audio channel 'PluginRackHost-...' unavailable (err 0); rendering in-process
```

So hosts scan+instantiate OK, print `HOSTREADY`, then fail to open the shared-memory audio
block (`PluginRackHost-<hash>`) and exit 1. The app falls back to in-process DSP on the
audio thread, which overloads the realtime callback and causes the sudden close / instability.

Secondary issue: AppContainer launch fails (code 2) because the app lives under OneDrive;
host falls back to normal/low-integrity token. Not the crash trigger.

## Root cause hypothesis

IPC hand-off is broken: the section created in `RestrictedHost::launch`
(`CreateFileMappingW(INVALID_HANDLE_VALUE, ..., sharedName)`) is not reopened by
`RestrictedHost::runHost` (`OpenFileMappingW(..., mappingName)`). Likely handle lifetime,
argument indexing, ACL/namespace, or size mismatch. `err 0` (not error 2) suggests the
failure is in `MapViewOfFile`/size, not "not found".

## Steps

### 1. Make the failure observable
- In `runHost`: log exact `mappingName`, OpenFileMapping result + GetLastError, MapViewOfFile
  result + GetLastError, and `sizeof(SharedAudioBlock)`.
- In `launch`: log created `sharedName` and MapViewOfFile result.
- Rebuild, run one plugin, read captured `HOSTERROR` line.

### 2. Fix shared-memory hand-off
Apply whichever the diagnostic confirms:
- **Args**: verify `Main.cpp` (`hostIndex+3` = mapping, `+4` = state) vs `launch()` array
  (`file, uniqueId, sharedName, state`).
- **Handle lifetime**: keep the mapping handle open in `RestrictedHost` instead of closing
  it right after `MapViewOfFile`; optionally `DuplicateHandle` into the child.
- **ACL/namespace**: grant the host token access to the section, or use `Local\` consistently.
- **Name stability**: derive once in app, pass verbatim; never recompute in host.

### 3. Make host death non-fatal
- Detect death on a timer/background thread, set atomic `hostDead` flag.
- `processBlock` reads only the flag; never retries a dead host every block.
- Ensure fallback never mutates the live buffer on a failed block.

### 4. Prevent sudden close
- Log-and-skip hosts that exit during scene restore instead of propagating exceptions.
- Keep `setApplicationCrashHandler` covering the audio thread (already in `initialise`).
- Add an audio-thread stall watchdog that logs instead of letting the OS terminate.

### 5. AppContainer
- Either grant container access to the app folder, move out of OneDrive, or make the
  low-integrity/normal-token fallback the default with clear logging.

### 6. Verification
- Smoke test: single `--test-plugin` run; assert `sandboxed host started … (low integrity)`
  + `prepare (sandboxed host)` for every plugin and `audio: devices open`, with no
  `host died` lines.
- Run core test suite (`ctest`).
- Manually reproduce with `--plugin-host <vst3> 0 <sharedName> -` against a pre-created section.

### 7. Delivery
- Commit diagnostic + fix + tests, rebuild `PluginRack.exe`.
- User must close all existing PluginRack windows and launch the fresh binary (stale
  builds have caused confusion before).

## Files likely touched
- `app/src/RestrictedHost.cpp`, `RestrictedHost.h`
- `app/src/Main.cpp`
- `app/src/PluginHost.cpp`, `PluginHost.h`
- `app/src/PluginHostIpc.h`

## Outcome

Root cause was **handle lifetime**, not a bad name or bad arguments: a
pagefile-backed section's name is only resolvable while a handle to it is open.
`launch` closed the `CreateFileMappingW` handle immediately after
`MapViewOfFile`, so every host's `OpenFileMappingW` failed with
`ERROR_FILE_NOT_FOUND (2)`. `NamedSection` now keeps the create handle open for
the life of the host.

Two related fixes were needed for the repaired channel to be safe:
- Each hosted instance gets its own channel name suffix. Two slots (or the
  `--test-plugin` probe) can host the same VST3 at once; a path-only name made
  them share one audio block.
- Hosts watch the app process and exit with it, so a `TerminateProcess` shutdown
  cannot leave orphaned hosts locking the exe.

The optional audio-thread stall watchdog was skipped: it cannot preempt a stuck
callback, and the mass-death trigger that made the fallback overload the audio
thread is gone. The existing `setApplicationCrashHandler` is unchanged.

## Status
- [x] Step 1 diagnostic logging — exact names/sizes/errors confirmed the name vanished after the create handle closed
- [x] Step 2 fix — `NamedSection` keeps the create handle open; host opens the channel before scanning and fails fast
- [x] Step 3 host-death handling — background watcher sets an atomic flag; `processBlock` never polls the OS
- [x] Step 4 crash prevention — crash handler kept; stall watchdog skipped (see Outcome)
- [x] Step 5 AppContainer — normal-token process isolation is the explicit default; already-existing profile is success
- [x] Step 6 verification — 314/314 tests; `--test-plugin` smoke: every scene host ready, audio pumped, exit 0, no leftovers
- [x] Step 7 delivery — committed and rebuilt `PluginRack.exe` / `PluginRack.pdb` in the repo root
