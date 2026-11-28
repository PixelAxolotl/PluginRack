# PluginRack improvement plan (for implementation agent)

## Goal
Address the highest-impact reliability and UX issues found in the 2026-11-16 `ponytail-audit` pass without changing user-facing behavior except where noted (responsiveness, clearer failure reporting).

## Status (2026-11-16)

- **Done:** #3 crash reporting hardened (`app/src/Main.cpp`, `app/src/CrashContext.h`),
  #2 concrete lock-held logging removed (`AudioEngine::handleClockLoss`), and #1
  scene restore staged (`AudioEngine::beginSceneRestore` + `MainComponent::applySceneAsync`):
  one plugin is instantiated per message-loop turn, with progress in the toolbar and the
  strip swap committed in one brief step at the end. The blocking `restoreScene` remains
  for diagnostics/tests and drains the same queue synchronously.
- **No code change needed:** #4 — every in-session save path already surfaces its
  error via `MainComponent::showError`; only the quit-time autosave note is
  deferred to the next launch, which is unavoidable because shutdown terminates
  the process. #5 — orphan containment already exists, and rights reduction is a
  separate project that is already documented as a trust boundary.
- **Retracted:** the `PluginLibrary` lifetime item (#6) — the class derives from
  `juce::DeletedAtShutdown` (`app/src/PluginLibrary.h:15`), so JUCE deletes it at
  shutdown.
- **Verification for #1:** build + full test suite green (`1150187 assertions in 526
  test cases`); `--test-load-save "M Audio Duo ASIO 4ms"` restored a scene while the
  message thread stayed alive, and the startup path logged each staged plugin
  (`scene: instantiating ... (n of total)`) before opening devices.

## Scope / non-goals
- In scope: scene restore responsiveness, audio callback lock contention, crash-report hardening, save/autosave failure visibility, plugin isolation hardening plan.
- Out of scope: new features, UI redesign, dependency upgrades, broad refactors unrelated to the findings below.
- Preserve existing scene format/compatibility and current plugin-host IPC behavior unless a change is explicitly required.

## Build & verification
After **every source or configuration change**, run from repo root:
```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1
```
Expected artifact: `PluginRack.exe` in repo root. Report build/copy failures explicitly; do not claim completion with a stale executable.

Recommended targeted verification (as available in this environment):
- `ctest` / `pluginrack_tests` targets relevant to touched areas (scene codec/validator, device/stream/health/ladder tests as applicable).
- Manual smoke checks when audio devices/plugins are available: restore large scene, add/remove plugin, open/close devices, induce save failure, verify startup notice.

---

## 1) Must fix — scene restore must not block the message thread

**Status: done.** The chosen design keeps plugin creation on the message thread
(as the constraint below requires) but stages it: `AudioEngine::beginSceneRestore`
builds the new strips, queues every insert, and a `juce::Timer` instantiates one
plugin per message-loop turn (`loadNextPendingInsert`). `commitSceneRestore` then
performs the same brief swap/device hand-off the old blocking path used. The UI
(`MainComponent::applySceneAsync`, SavesView) disables saves while the restore is
active, shows `Loading scene... done/total` in the toolbar, and refreshes views once
at completion. `restoreScene` stays synchronous for diagnostics/tests by draining
the same queue, so no second code path exists. The static restore test was updated
to pin the invariants: saved state is queued in prepare, only the staged loader
calls `loadPlugin`, and the commit opens devices only after the queue is empty.

### Problem
`MainComponent::applyScene` runs on the message thread and calls `audioEngine.restoreScene(...)` (`app/src/MainComponent.cpp`, around restore/apply path). `AudioEngine::restoreScene` instantiates plugins during restore (`app/src/AudioEngine.cpp:~2185+`, plugin instantiation logged around `2353-2355`). Slow VST3 scan/load/instantiate can freeze the UI for seconds and makes recovery from bad plugins harder.

### Current behavior to preserve
- Scene validation semantics and error reporting (invalid/missing/partial restore messages).
- Strip/state consistency after restore (no mixed old/new strips exposed to UI).
- Existing async plugin add flow pattern (`AudioEngine::addPluginAsync`, `app/src/AudioEngine.cpp:~1209+`) which scans/instantiates off-thread and adopts on message thread.

### Implementation approach
1. **Introduce async/staged restore API** in `AudioEngine` (e.g. `restoreSceneAsync(scene, openDevicesNow, progressCb, doneCb)`), keeping current sync method only if still needed internally/tests — but UI path must stop blocking.
2. **Move heavy work off message thread**:
   - Parse/validate already done by `SceneBridge` before apply; keep cheap state prep on message thread.
   - Run plugin file scan + `PluginHost::instantiate`/plugin creation work on a background worker (serialized per restore job; cancel/ignore stale jobs if a newer restore starts).
   - Marshal only adoption/mutation of engine-owned structures and UI-facing publication back to the message thread via `juce::MessageManager::callAsync` (or existing safe dispatch pattern).

   **Constraint found during implementation:** `PluginHost::addPluginAsync` deliberately
   scans in a child process on a worker, then instantiates on the message thread
   (`app/src/AudioEngine.cpp:1209-1237`). `PluginHost::instantiate` calls JUCE
   `createPluginInstance` and is kept on the message thread on purpose, so moving
   it to a worker needs a COM/thread-affinity audit first. Also, `restoreScene`
   currently loads every plugin before the device handoff
   (`tests/test_underrun_paths.cpp:483-494`); committing strips with unloaded
   slots would let an autosave capture empty plugin state unless each slot keeps
   its saved state until the async load finishes.
3. **Stage UI updates** (`MainComponent::applyScene` / `SceneBridge::onRestoreRequested` flow):
   - Enter loading state immediately (reuse `sceneLoading_`/existing loading indicators), disable conflicting actions if needed.
   - Apply structural scene data and show progress/status; avoid long synchronous loops on UI thread.
   - On completion/failure, refresh views once with final errors (including plugin load errors) and clear loading state.
4. **Safety/cancellation**:
   - Guard against overlapping restores and teardown while restore in flight (alive token/weak ref + generation/id check).
   - Ensure old strips/plugins/editors are released on the correct thread per JUCE/plugin-host ownership rules (follow existing editor release in `applyScene`: `releaseDockedEditor`, mixer/editor teardown before swap).

### Acceptance criteria
- Restoring a scene with multiple/slow plugins keeps the window responsive (repaints/input processed) during load.
- Final restored state matches current behavior for valid scenes; plugin load errors are still reported.
- No crashes/regressions on repeated restore, restore-during-startup, or app close during restore.

### Files likely involved
- `app/src/AudioEngine.h/.cpp` (restore orchestration, async job, state publication)
- `app/src/MainComponent.h/.cpp` (apply/restore UI flow, loading state)
- `app/src/SceneBridge.*` (restore entry points if callback shape changes)
- Possibly `app/src/PluginHost.*` if instantiate/adopt threading assumptions need tightening

---

## 2) Should fix — prevent audio dropouts from `stateLock` contention

**Status:** the original "long UI operations hold the lock" claim did not hold up
on review — restore builds strips outside the lock. The one concrete case found
was a `juce::Logger::writeToLog(...)` call inside the clock-failover critical
section (`app/src/AudioEngine.cpp:3252`), which made every render callback pay
for file IO with silence. Fixed: the output name is copied under the lock and
the log call moved after it (`AudioEngine::handleClockLoss`). No snapshot
rewrite was justified by the evidence.

### Problem
`AudioEngine::render` uses a timed try-lock on `stateLock` (`BriefTryLock(..., jlimit(1,16,frames/16))`) and outputs silence on lock failure (`app/src/AudioEngine.cpp:~169-189, ~450-470`). Long UI/config operations holding `stateLock` can cause audible dropouts.

### Implementation approach (incremental, lowest-risk first)
1. **Audit and shrink critical sections**:
   - Identify UI/non-realtime methods holding `stateLock` for long work (device changes, scene apply, strip rebuilds, allocations/logging) and move heavy/non-shared work outside the lock.
   - Ensure logging, plugin scan/instantiate, file IO, and device open/close never occur while `stateLock` is held for audio-relevant state.
2. **Snapshot/publish pattern for audio path** (preferred durable fix):
   - Maintain an audio-thread-readable snapshot of render-critical state (channels/outputs/routes/active devices/processing graph refs) updated atomically or via double-buffer/swap on message/control thread.
   - Let `render` read the snapshot without contending with UI mutations; keep `stateLock` for control-plane operations only if still needed.
3. **Validate callback safety**:
   - No allocations, blocking waits, or UI/message-thread calls on audio callback path.
   - Preserve existing underrun/miss accounting semantics where meaningful.

### Acceptance criteria
- Normal UI actions during playback (where safe/supported) do not cause silence due to lock misses.
- No data races; render path remains real-time safe.

### Files likely involved
- `app/src/AudioEngine.h/.cpp`
- Potentially `app/src/DeviceStream.*` / routing/strip structures if snapshot boundaries change

---

## 3) Should fix — make crash reporting crash-safe/minimal

**Status: done.** The handler now uses a startup-resolved log directory, a
fixed stack buffer, and Win32 `CreateFileW`/`WriteFile`; the cheap header is on
disk before any allocating call (plugin path, backtrace), and
`PluginLoadGuard::block` runs last. `crash_context::currentRenderLabel()` adds a
no-allocation accessor for the render label. Verified with
`PluginRack.exe --test-crash-report Test.vst3`: the new report contains the
timestamp, build, exception code, fault address, render label, and backtrace in
the same file/location format.

### Problem
`writeCrashReport` (`app/src/Main.cpp:~57+`, registered at ~230) performs string building, plugin-load bookkeeping, stack capture, stderr output, directory creation, and file writes inside the crash handler. This is risky after memory corruption or re-entrant faults.

### Implementation approach
1. **Minimize handler work**:
   - Capture only essentials: exception code (Windows), faulting context already available, loading-plugin path (from `PluginLoadGuard` state) and render context (`crash_context`) using preallocated/static buffers where possible.
   - Avoid heap-heavy formatting (`juce::String` concatenation), avoid complex path logic beyond fixed/precomputed report location, avoid logging frameworks.
2. **Safe persistence**:
   - Precompute/open crash report directory/path at startup if feasible; on crash, do a single straightforward write/flush (e.g. Win32 `CreateFileW/WriteFile` with fixed buffer) and exit.
   - Keep stderr write best-effort and minimal.
3. **Preserve useful diagnostics** without handler complexity:
   - Record richer context continuously during runtime (last render/plugin load transitions) into crash-safe storage so the handler only dumps it.

### Acceptance criteria
- Crash handler performs no unbounded allocation/complex IO; report still includes timestamp/build basics (or prewritten build id), exception code, and last known plugin/render context when available.
- Handler remains registered and does not regress normal startup/shutdown.

### Files likely involved
- `app/src/Main.cpp`
- `app/src/crash_context.*`, `app/src/PluginLoadGuard.*` (only if storage format/API needs to become crash-safe)

---

## 4) Should fix — improve save/autosave failure visibility

**Status: no change needed.** Every live save path already surfaces its error
through `MainComponent::showError` (`app/src/MainComponent.cpp:305-306`,
`690-691`, `703-704`, `719`, `725-726`, `783-787`). The only deferred case is
the shutdown autosave (`app/src/MainComponent.cpp:791-798`), which writes
`last-save-error.txt` and shows it on the next launch; a dialog is impossible
because shutdown terminates the process immediately afterwards.

### Problem
`SceneBridge` records/reports save failures and `MainComponent` writes a failure note on exit autosave failure and shows `takeSaveFailureNote()` on startup (`app/src/SceneBridge.cpp`, `app/src/MainComponent.cpp:~898-914`). Failures during session/at shutdown can still be missed until relaunch.

### Implementation approach
1. **Immediate in-session surfacing**:
   - After any `save`, `saveLast`, or autosave attempt triggered during runtime, if result/error non-empty show a visible status/toast/banner with exact reason (path/operation + OS error text where available).
   - Do not show modal dialogs in paths that must complete quickly unless already modal-safe.
2. **Startup prominence**:
   - Keep/strengthen startup handling of `SceneBridge::takeSaveFailureNote()` so the message is clearly visible (not only transient log) before/alongside fallback restore.
3. **Consistency**:
   - Ensure all save paths return and propagate actionable errors (disk full, permission denied, invalid name/path, move/rename failures) without silent drops.

### Acceptance criteria
- User sees save/autosave failures when they happen (except unavoidable process-termination cases) and on next launch if a quit-time save failed.
- No duplicate/noisy alerts for the same failure event.

### Files likely involved
- `app/src/MainComponent.*` (UI surfacing)
- `app/src/SceneBridge.*`
- Save-related views/controllers if they currently swallow errors (e.g. `app/src/SavesView.*`)

---

## 5) Should fix — plugin isolation hardening (defense-in-depth)

**Status: reviewed; no further code change recommended now.**
- Orphan containment already exists: the child watches the app's process handle
  and terminates when it goes away (`terminateWhenParentExits`,
  `app/src/RestrictedHost.cpp:36-51`), so a crashed app cannot leave a host
  holding the exe or the named channels. A Job Object kill-on-close would
  duplicate that, and `juce::ChildProcess` exposes no process id to assign one.
- Rights reduction is the remaining gap, and it is already stated in the UI and
  README. An AppContainer launch needs a profile, ACLs on plugin directories,
  and a fallback for plugins that touch files or the network - a separate
  project with real compatibility risk, not a safe drive-by change.

### Problem
VST3 plugins run out-of-process (`RestrictedHost::runHost`, `PluginProcessHost`, IPC) which contains crashes/hangs, but the child runs with normal user rights (README trust note). Malicious plugins are not sandboxed.

### Implementation approach (staged; Windows-specific constraints apply)
1. **Document current boundary clearly** in code/comments and user-facing warning text (fault isolation != security sandbox).
2. **Add best-effort OS containment** for plugin host child process (evaluate feasibility in this codebase/toolchain):
   - Assign Job Object limits (kill-on-close, active process/time/memory limits as appropriate for audio use).
   - Run with restricted token / lowered integrity / AppContainer profile if compatible with required VST3/plugin loading behavior.
   - Constrain child environment/handles where possible.
3. **Keep compatibility escape hatches** behind explicit settings only if containment breaks legitimate plugins; default to safer mode.

### Acceptance criteria
- Plugin crashes still isolated as today.
- Child process runs with measurable restriction (tests/logged evidence) without breaking supported plugin-host startup path.
- Any incompatible restriction is opt-in and documented.

### Files likely involved
- `app/src/PluginProcessHost.*`, `app/src/RestrictedHost.cpp`, `app/src/Main.cpp` (`--plugin-host` path), `app/src/PluginHost.*`
- `README.md` / UI trust messaging where applicable (treat as config/doc change; build step policy applies if source/config touched per repo rules)

---

## Nice to have — PluginLibrary lifetime cleanup

**Status: retracted.** `PluginLibrary` derives from `private juce::DeletedAtShutdown`
(`app/src/PluginLibrary.h:15`); `getInstance()` returns a process-lifetime object
that JUCE deletes during shutdown, as the comment says. There is no leak to fix.

### Problem
`PluginLibrary::getInstance()` heap-allocates a never-deleted singleton (`app/src/PluginLibrary.cpp:~95-120`), which is harmless at runtime but noisy for shutdown/leak tooling.

### Implementation approach
- Replace with function-local static instance or app-owned lifetime object with deterministic shutdown; preserve thread-safe lazy init and existing scan-thread join/cleanup behavior in destructor.

### Acceptance criteria
- No behavior change in plugin scanning/caching; clean shutdown under leak/sanitizer-style checks if used.

---

## Suggested implementation order
1. ~~Crash-report hardening~~ - done.
2. ~~Save/autosave failure visibility~~ - reviewed, already covered.
3. ~~Audio `stateLock` contention reduction~~ - one concrete case fixed.
4. ~~Async/staged scene restore~~ - done (staged, one plugin per message-loop turn).
5. ~~Plugin host containment hardening~~ - reviewed, existing coverage documented.
6. ~~PluginLibrary lifetime cleanup~~ - retracted.

## Done checklist per PR/change
- [ ] Source/config changes built via `tools\build-and-copy.ps1` and root `PluginRack.exe` refreshed.
- [ ] Relevant automated tests run/passed (list exact commands/results).
- [ ] Manual smoke notes recorded (or explicitly unavailable).
- [ ] No unrelated refactors; behavior changes documented above.
- [ ] Commit messages follow Conventional Commits and contain no AI attribution trailers.

---

## Re-audit fixes (follow-up pass)

### UI and saves
- **Device picker use-after-free** (`app/src/MixerComponent.cpp`): the async
  device open now holds a `juce::Component::SafePointer<OutputStrip>` and bails
  when the strip was rebuilt away while the driver was opening.
- **Failed-plugin state survives save** (`app/src/AudioEngine.*`): `InsertSlot`
  keeps `savedPluginState`, `copyInserts` writes it back when the host has no
  live instance, and `makeScene` only overwrites state from a loaded plugin.
  A missing plugin no longer erases its saved state on autosave.
- **Save identity after rename/delete** (`app/src/SavesView.*`,
  `app/src/MainComponent.*`): rename/delete report the new identity through
  `onSaveIdentityChanged`, so toolbar Save cannot recreate the old file.
  Rename/Delete are disabled during a load.
- **Reset to Defaults during staged restore** is refused with the same message
  as a save.
- **`.json` save-name collision** is rejected: `Mix` and `Mix.json` can no
  longer target the same file.

### Engine
- **Restore buffer sizing**: staged strips are resized to the current
  `maxBlockFrames_` during commit, because a clock reopen can change the block
  while plugins are still loading.
- **Commit waits for device opens**: the restore timer keeps polling while
  `isOpeningDevices()` is true, so a commit never frees strips a device worker
  still references.
- **Fault recording off the audio thread**: `PluginFaultGuard::record` now runs
  through `MessageManager::callAsync`, not inline in the render callback.
- **Input capture trackers under `stateLock`**: health thresholds and render
  block frames are set before the lock is released, so a render cannot read a
  half-updated tracker.
- **ASIO retarget rollback**: `forceAsio` restores the previous input
  assignments when the clock move is refused.

### Core DSP and validation
- **Multiband heap overflow**: scratch is reserved for the maximum crossover
  count, so `prepare()` before `setEffect()` can no longer undersize it.
- **Shelf EQ NaN**: the RBJ radicand is clamped and non-finite coefficients
  deactivate the band instead of poisoning the strip.
- **Validator gaps**: de-esser attack/release, multiband makeup/input gain/knee
  are range-checked, and the crossover count must always be `bands - 1`.
- **Wide outputs**: channels above stereo are cleared instead of replaying
  whatever the device buffer held.

### Regression tests added
- `tests/test_eq_engine.cpp`: extreme shelf stays finite.
- `tests/test_multiband.cpp`: six bands reserve scratch for five crossovers.
- `tests/test_scene_validator.cpp`: de-esser time, multiband gain, crossover
  count rejection.
- `tests/test_underrun_paths.cpp`: ASIO retarget/rollback literal.

Verified: `tools\build-and-copy.ps1` green, `pluginrack_tests.exe`
`All tests passed (1150187 assertions in 526 test cases)`.

Engine/plugin-host follow-up audit (after the fixes above) reported five
issues: device opener mid-probe vs. commit, stale buffer sizing at commit,
fault I/O on the audio thread, capture trackers published before setup, and
an ASIO retarget without rollback bookkeeping. All five were already fixed in
this pass; the reviewer was reading the pre-fix revision.
