# Plugin Fault Containment — Contingency Plan

| Field | Value |
| --- | --- |
| Status | **Contingency — do not implement yet.** Fix 1 (`5132a41`) removed the crash that motivated this plan. |
| Trigger to reopen | A plugin still faults `PluginHost::processBlock` on the audio thread, or a new teardown path appears that `releaseEditorsNow()` does not cover. |
| Scope | Keep the app and the other strips alive when one plugin faults. Not OS-level sandboxing. |
| Spec | None yet. Phase 0 is the spec; do not write task-level code before it is approved. |

## Why this is contingency rather than next

The two crashes on 13 Nov were:

1. `~PluginHost` → Ozone Imager 2 `terminate()` → heap AV while loading the
   M-Audio save.
2. `RackView::destroyEditorWhenIdle` → `VST3PluginWindow` destructor after the
   processor was gone.

Both were the same lifecycle bug: editors were queued for deletion 80 ms later,
then the scene swap destroyed the processors immediately. `5132a41` releases
every rack editor synchronously before the swap and leaks (with a log) instead
of deleting an editor whose processor is already gone. The exact repro,
`--test-load-save "M Audio Duo ASIO 4ms"`, now loads at 8.4 s, logs "still
alive", and exits normally with no crash file.

The residual risk is different: an AV **inside** `plugin->processBlock` cannot
be caught in-process. The `processAudio` crash family from 10 Nov is that case,
and it would still kill the app if it recurred.

## Approaches

**A. DSP in a child process, editor in-process (recommended).**
The child owns the `IAudioProcessor` and renders. The in-process instance exists
only to own the editor and parameter objects. Audio, parameters, state and
meters cross the existing shared-memory channel. The old `RestrictedHost` path
did the audio half but left the in-process instance dead, which is why meters
went flat and edits were inaudible; this plan fixes that by making the child the
only DSP and mirroring the editor's parameters/state into it. On child death the
insert bypasses to dry audio, stays named, and is marked failed.

**B. Full child process with a cross-process editor.**
The child owns both audio and editor; the editor HWND is embedded via
`SetParent`. Largest change, fragile with VST3 editor resizing and focus. Not
recommended.

**C. Quarantine/blocklist alone.**
Only works if a crash can be attributed to a plugin. A hard process death
leaves no reliable attribution in-process. With approach A the child's exit code
identifies the plugin, so quarantine becomes a natural Phase 3 rather than a
standalone design.

**Recommendation:** A, with C as its last phase.

## Phase 0 — Spec (required before any tasks)

Decide and write down:

- IPC contract: block format (float32, max frames), deadline, parameter
  messages, full state vs deltas, meter messages, latency reported by the child.
- Lifecycle: spawn, handshake, per-block timeout, missed blocks, child crash,
  restart policy, app shutdown.
- Fallback: if the child cannot start, run in-process (today's behaviour) with a
  visible warning; never silently mute the insert.
- Compatibility: ASIO single-client, duplex mode, 64-bit state sizes, sample
  rate/buffer changes, scene restore.
- Security: the helper currently runs with the user token; state that AppContainer
  isolation is still out of scope.

## Phase 1 — Vertical slice (one insert, one plugin)

- Introduce `PluginProcessHost` behind `PluginHost`:
  `start(path, uniqueId, state)`, `submitBlock(const AudioBuffer<float>&)`,
  `fetchBlock(AudioBuffer<float>&, double& rate, int& block)`,
  `setParameter(id, value)`, `requestState()`, `isAlive()`, `exitCode()`.
- Child process reuses the dormant `RestrictedHost` audio channel (or a minimal
  equivalent); it creates the plugin with `createPluginInstance` and renders.
- In-process instance: created for the editor only, never `processBlock`ed.
  Mirror editor parameter changes and the initial state to the child.
- Timeout: if the child misses N consecutive blocks, bypass the insert for that
  block and log the miss; do not double-process.
- Tests:
  - kill the child while audio runs → app stays alive, the insert outputs dry
    audio, the other strips keep rendering, and the insert reports a failure;
  - a normal plugin renders identically through the child (null test against the
    in-process path);
  - scene save while the child is alive returns the plugin state, and a timeout
    falls back to the last known state.

## Phase 2 — Editor, parameters and meters

- Parameter bridge both ways (editor → child, child → editor), coalesced per
  block; do not block the message thread on the child.
- Meters: child sends its input/output peak per block; the strip meter reads
  that instead of the in-process tap.
- State: save/restore through the child with a bounded timeout.
- Latency: the child reports its plugin latency; fold it into the existing
  latency compensation.

## Phase 3 — Quarantine and UI

- On a child crash, record the plugin path and exit code for the session; do not
  re-spawn until the user clicks Reload.
- Optionally persist quarantine in `%APPDATA%/PluginRack/plugins/faults.json`
  (path, exit code, time) with a Settings entry to clear it.
- Strip shows "Plugin crashed (0x…)" and a Reload action; audio stays dry.

## Risks

| Risk | Mitigation |
| --- | --- |
| Added block of latency and IPC overhead | Phase 1 null test; measure per-plugin and keep the in-process path for plugins that opt out |
| Editor and child state drift | Mirror parameters on every change, resend full state on mismatch, log divergences |
| ASIO single-client / duplex | Route the child's device access through the same opener policy; test with the M-Audio |
| Child startup cost on scene load | Start in parallel with the other inserts, same as the device opener |
| False attribution when several plugins run | One child per insert in Phase 1; attribute by child, not by module |

## Verification

- Existing `--test-plugin-audio` can host a known-bad plugin; add a diagnostic
  that kills the child after N seconds and asserts the app still renders.
- Reproduce the 10 Nov `processAudio` family in the child and confirm the app
  survives with the insert bypassed.
- Full `ctest` stays green; the in-process path remains the default until the
  child path passes the null test.

## Out of scope

- AppContainer/OS sandboxing.
- Cross-process editor embedding.
- Making the child the default for every plugin before Phase 1's null test.
