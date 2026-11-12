# Plugin Fault Containment — Contingency Plan (v2, actionable)

| Field | Value |
| --- | --- |
| Status | **Contingency — do not implement yet.** Fix 1 (`5132a41`) removed the crash that motivated this plan. |
| Trigger to reopen | A plugin still faults `PluginHost::processBlock` on the audio thread, or a new teardown path appears that `releaseEditorsNow()` does not cover. |
| Scope | Keep the app and the other strips alive when one plugin faults. Not OS-level sandboxing. |
| Spec | **Drafted, awaiting sign-off:** `docs/superpowers/specs/2026-11-13-fault-containment-ipc.md` (Phase 0, no code). Phase 1 does not start until §5.6 is fully checked. |
| This revision | v2 adds **WHERE** (exact files/functions) and **HOW** (step-by-step patches, commands, exit criteria) to every phase. v1 text is preserved and marked. |

> How to read this file: each phase has **Goal → WHERE → HOW → Verify →
> Exit criteria**. `WHERE` names files and functions in
> `PixelAxolotl/PluginRack@master`. `HOW` is the ordered patch sequence.
> `Verify` is copy-paste commands. Nothing here runs until the trigger fires.

**Changelog v1 → v2**

- Added §1 repo map (every file this plan touches, and in which phase).
- Added §5 Phase 0 spec template with exact struct/message/timeout decisions to record.
- Split Phases 1–3 into numbered tasks, each with WHERE + HOW + code sketch + verify.
- Added rollout flags (§9), verification matrix (§10), and appendices with
  header sketches and a command cheat sheet.
- Fixed v1 gaps: 64-bit state sizes, sample-rate/buffer-change flow, ASIO
  single-client routing, scene-restore ordering, and the "never silently mute" fallback rule are now explicit tasks, not footnotes.

---

## 0. TL;DR decision tree

```
Did a plugin fault INSIDE plugin->processBlock on the audio thread?
├─ NO — crash is in an editor dtor / terminate() / scene swap
│   └─→ Extend Fix 1 (releaseEditorsNow) to that path. Do NOT start this plan.
└─ YES — crash-*.txt symbolizes through PluginHost::processBlock
    └─→ Reopen this plan:
        1. Write Phase 0 spec (§5) and get it approved. No task code yet.
        2. Phase 1: one insert, one plugin, child DSP + in-process editor (§6).
        3. Phase 2: params/meters/state/latency bridge (§7).
        4. Phase 3: quarantine + strip UI (§8).
```

**Recommendation (unchanged from v1): Approach A** — DSP in a child
process, editor in-process — with C (quarantine) as its last phase.
B (cross-process editor) is rejected. Rationale in §4.

---

## 1. Repo map — WHERE everything lives

All paths relative to repo root. Build: `cmake -B build/dev -G Ninja`
then `cmake --build build/dev --target pluginrack_app`
(requires `JUCE_DIR` + `VST3_SDK_DIR`; see `README.md`).

| File | Role today | Touched in |
| --- | --- | --- |
| `app/src/PluginHost.h` / `.cpp` | In-process VST3 owner: `loadFromFile`, `instantiate`, `adopt`, `prepareToPlay`, `processBlock`, `createEditorForDock`, `getStateAsBase64`, `getLatencySamples`. `processBlock` is the fault site. | Phase 1 (delegate to child), Phase 2 (param/state bridge) |
| `app/src/PluginHostIpc.h` | Shared-memory layout `SharedAudioBlock` (float32 stereo interleave, `kMaxBlockFrames = 8192`), sequence handshake, event-name helpers. **Transport already exists.** | Phase 0 (extend spec), Phase 1 (param/meter/state fields) |
| `app/src/RestrictedHost.h` / `.cpp` | Child-process launcher + audio channel: `launch`, `submitAudioBlock`, `fetchAudioBlock`, `runHost` (the `--plugin-host` entry), watcher thread, exit code. Dormant for DSP today. | Phase 1 (revive as DSP host) |
| `app/src/NamedSection.h`, `NamedEvent.h` | Shared-memory section + auto-reset events (`-req`/`-rsp`). Timer-tick-independent wake-ups. | Phase 0 (reuse as-is), Phase 1 (new param/state events if needed) |
| `app/src/AudioEngine.h` / `.cpp` | Mixer: `InsertSlot{pluginHost, bypassed, latencySamples}`, `render(frames)`, `restoreScene`, `makeScene`, `addPluginAsync`, `removePlugin`, latency report. | Phase 1 (per-block timeout + dry bypass), Phase 2 (latency fold-in) |
| `app/src/RackView.h` / `.cpp` | Rack UI: docked editor, `hiddenPreviews`, `doomedPreviews`, `releaseEditorsNow()`, `destroyEditorWhenIdle()`, insert cards + thumbnails, `knobWatcher`. | Phase 1 (failure badge + Reload), Phase 2 (meter source switch) |
| `app/src/Main.cpp` | CLI: `--plugin-host`, `--scan-plugin`, `--test-plugin`, `--test-plugin-audio`, `--test-load-save`, `--clear-blocked-plugins`, crash handler. | Phase 1 (new `--test-kill-child`, extended `--test-plugin-audio`) |
| `app/src/MainComponent.cpp` | Scene-load path: must call `releaseEditorsNow()` before `restoreScene`. | Phase 1 (spawn children in parallel on load) |
| `app/src/SceneBridge.cpp`, `core/.../Scene.*` | Scene codec/validation (`kSchemaVersion`, insert state blobs). | Phase 1 (child state save with timeout), Phase 2 (full restore) |
| `app/src/SettingsView.cpp`, `app/src/Theme.h` | Settings page + shared UI vocabulary. | Phase 3 (quarantine list + Clear) |
| `app/src/ChannelStrip.cpp`, `MixerComponent.cpp` | Strip meters (read `peakL/peakR` atomics today). | Phase 2 (read child peaks) |
| `app/src/InsertHost.h`, `InsertMiniView.*` | Insert card interface + thumbnails. | Phase 3 (crashed badge) |
| `tests/test_underrun_paths.cpp` + `tests/*` | Catch2 suite incl. source-invariant tests that protect the audio path. | Every phase (extend, keep green) |
| `tools/build-and-copy.ps1` | Rebuild + copy exe to root (git-ignored). | All manual verification |
| `%APPDATA%\PluginRack\logs\` | `PluginRack.log`, `crash-*.txt` | Verification |
| `%APPDATA%\PluginRack\plugins\faults.json` | **Does not exist yet.** Phase 3 quarantine store. | Phase 3 |

**Grep cheat sheet (run from repo root):**

```powershell
rg -n "processBlock" app/src/PluginHost.cpp app/src/AudioEngine.cpp
rg -n "releaseEditorsNow|destroyEditorWhenIdle|doomedPreviews" app/src/RackView.*
rg -n "runHost|--plugin-host" app/src/Main.cpp app/src/RestrictedHost.cpp
rg -n "SharedAudioBlock|requestEvent|responseEvent" app/src/PluginHostIpc.h app/src/RestrictedHost.*
rg -n "test-plugin-audio|test-load-save|clear-blocked" app/src/Main.cpp
rg -n "getStateAsBase64|makeScene|restoreScene" app/src/AudioEngine.cpp app/src/SceneBridge.cpp
```

---

## 2. Why this is contingency rather than next (v1, kept)

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

**v2 WHERE to confirm Fix 1 is in place (before doing anything else):**

- [ ] `rg -n "releaseEditorsNow" app/src` hits `RackView.h`, `RackView.cpp`,
      and the scene-load caller in `MainComponent.cpp`.
- [ ] `RackView.cpp::destroyEditorWhenIdle` checks `pluginHostAlive(host)`
      and logs + leaks when false.
- [ ] `.\PluginRack.exe --test-load-save "M Audio Duo ASIO 4ms"` exits 0 with
      no new `crash-*.txt` (see `report.md` §7 for the full checklist).

---

## 3. Approaches (v1, kept + v2 decision detail)

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

### v2: decision matrix (why A wins)

| Criterion | A: child DSP + in-process editor | B: full child + SetParent editor | C: blocklist only |
| --- | --- | --- | --- |
| Survives AV in `processBlock` | ✅ child dies, app lives | ✅ | ❌ process already dead |
| Attribution (which plugin?) | ✅ by child (one child per insert) | ✅ | ❌ fault address only |
| Editor keeps working (resize, focus, scaling) | ✅ untouched — `RackView` bay, `applyDockedEditorFit`, `PluginKnobWatcher` unchanged | ❌ `SetParent` breaks VST3 resize/focus; high risk | n/a |
| Meters stay live | ✅ child sends peaks per block (Phase 2) | ✅ | ❌ |
| Edits audible | ✅ param mirror editor → child (Phase 2) | ✅ | ❌ |
| Change size | Medium (IPC + bridge + UI badge) | Large + fragile | Small but useless |
| ASIO single-client impact | None (child never opens devices — see §5.4) | None | None |

### v2: architecture sketch (Approach A)

```
TODAY (in-process — one fault kills everything)
─────────────────────────────────────────────────
AudioEngine::render ──► InsertSlot::pluginHost.processBlock ──► plugin->processBlock (VST3 DSP)
RackView docked editor ──► same instance (editor + params + meters)

APPROACH A (child DSP — one fault bypasses one insert)
─────────────────────────────────────────────────
Audio thread:  AudioEngine::render ──► PluginProcessHost::submitBlock(scratch)
                                                  │ shared memory (PluginHostIpc.h)
                                                  ▼
                                           child: RestrictedHost::runHost
                                                  │  plugin->processBlock in CHILD
                                                  ▼
                                          PluginProcessHost::fetchBlock ──► commit or dry-bypass
Message thread: in-process instance (adopt, never processBlocked)
                └─► owns editor (RackView bay unchanged) + params ──► mirror to child per block
Meters: child sends peakL/peakR per block ──► strip meter reads child peaks
State:  save/restore via child with timeout ──► fallback to last-known-good
Death:  watcher sees exit (e.g. 0xC0000005) ──► insert bypasses dry, badge "Plugin crashed (0x…)" + Reload
```

---

## 4. Global rules (v2 — apply to every phase)

1. **Never silently mute.** If the child cannot start, misses its budget,
   or returns garbage, the insert plays **dry audio** (input passes
   through) and the UI says why. Silence is only acceptable when the
   user bypassed the insert.
2. **Audio thread never blocks, never allocates.** All IPC waits are
   bounded (`≤ 50% of the block period`, see §5.1); all buffers are
   preallocated in `prepareToPlay`. Violation = `tests/test_underrun_paths.cpp`
   failure.
3. **One child per insert in Phase 1.** Attribution is by child, never
   by module. Shared children come later or never.
4. **Child never touches audio devices.** ASIO single-client means the
   child must not call `setChannelInputDevice` / `setOutputDevice` or
   open any `DeviceStream`. It renders buffers it is handed. (This is
   the most common way to break the M-Audio rig — see §5.4.)
5. **In-process path stays default until the Phase 1 null test passes.**
   Ship behind a flag (§9); per-plugin opt-out list for plugins that
   measure worse through IPC.
6. **MSVC `/W4 /WX` stays clean; `ctest` stays green at every commit.**
   No phase lands with a red suite.

---

## 5. Phase 0 — Spec (required before any tasks)

> **Phase 0 produces a document, not code.** Write it in
> `docs/superpowers/specs/YYYY-MM-DD-fault-containment-ipc.md`, get it
> approved, then start Phase 1. If any box in §5.6 is unchecked, Phase 1
> does not start.

### 5.0 WHERE to write it

- New file: `docs/superpowers/specs/<date>-fault-containment-ipc.md`
  (follow the existing `2026-10-28-pluginrack-design.md` format).
- Reference-only reads (no edits): `app/src/PluginHostIpc.h`,
  `app/src/NamedEvent.h`, `app/src/NamedSection.h`,
  `app/src/RestrictedHost.*`, `app/src/AudioEngine.h` (`InsertSlot`,
  `LatencyReport`), `app/src/Main.cpp` (`--plugin-host` args).

### 5.1 HOW: IPC contract — decide and write down

| # | Decision | Options / constraint | Suggested default |
| --- | --- | --- | --- |
| IPC-1 | Block format | float32 stereo interleave (existing) vs planar | **Keep interleave** (`SharedAudioBlock::interleave`) |
| IPC-2 | Max frames | `kMaxBlockFrames = 8192` today; ASIO 64–512, WASAPI ≤ 1920 typical | **Keep 8192**; assert `frames ≤ 8192` in `submitBlock` |
| IPC-3 | Per-block deadline | Must be `<` block period; audio thread must never wait a full period | **`min(50% of block period, 20 ms)`**; miss = dry-bypass that block + log |
| IPC-4 | Param messages | Piggyback on audio block vs separate ring | **Separate SPSC param ring** (64 slots, `paramId:uint32 + value:float + seq:uint64`) in the same mapping; coalesced per block, editor → child and child → editor |
| IPC-5 | State transfer | Full blob per save vs deltas | **Full blob** through a side channel (named pipe or second mapping), **bounded at 64-bit size** (`uint64_t` length prefix; today's `kMaxPluginStateBase64Chars = 1 MiB` becomes a documented limit, not an `int` truncation) |
| IPC-6 | Meter messages | Per-block peaks in the audio-block header vs separate message | **Header fields** `peakInL/R, peakOutL/R: float` written by the child before `responseSequence`; strip meter reads them (Phase 2) |
| IPC-7 | Latency report | Child reports `getLatencySamples()` per prepare + on demand | **`latencySamples:int32` header field** + `requestState`-style control line; folded into `LatencyReport::pluginMs` (Phase 2) |
| IPC-8 | Sample-rate / buffer changes | Who initiates re-prepare | **App initiates**: `prepareToPlay` sends control `{"prepare":{sr,bs}}`, child re-prepares before next block; child echoes `sr/bs` in every header so drift is detectable |
| IPC-9 | Sequence/handshake | Existing `sequence/responseSequence/blockSequence` torn-read guards | **Keep**, extend with `paramSeq` + `stateSeq` counters following the same even/odd discipline |
| IPC-10 | Wake-ups | Existing `-req`/`-rsp` auto-reset events | **Keep**; add `-param` event only if the param ring needs out-of-band wake (decide in spec, not in code) |

### 5.2 HOW: Lifecycle — decide and write down

| # | Decision | Suggested default |
| --- | --- | --- |
| LC-1 | Spawn | One `PluginRack.exe --plugin-host <file> <uniqueId> <mapping> <state\|->` per insert, launched from `PluginProcessHost::start` (message thread). Children start **in parallel** on scene load (same pattern as `openStripsDevicesAsync`). |
| LC-2 | Handshake | Child prints `HOSTREADY` on stdout within **10 s**; anything else (`HOSTERROR <reason>`) or timeout = launch failure → in-process fallback with visible warning (rule §4.1). |
| LC-3 | Per-block timeout | §5.1 IPC-3. Miss counter per insert; `N = 3` consecutive misses = declare the child wedged → dry-bypass + mark failed (do not kill yet — see LC-5). |
| LC-4 | Child crash | Watcher thread (existing `RestrictedHost` pattern: poll `ChildProcess::isRunning` every 100 ms, audio thread reads only an atomic) sets `hostDead`; next `render` bypasses dry, records `exitCode` (e.g. `0xC0000005`), notifies the message thread. |
| LC-5 | Restart policy | **No auto-restart.** `killAndReap()` + record in `faults.json` (Phase 3); re-spawn only on user **Reload** (prevents crash loops on a deterministically-bad plugin/state). |
| LC-6 | App shutdown | Today's `TerminateProcess` path must also reap children: enumerate owned children and `killAndReap()` **before** terminating (else orphaned hosts lock the exe — the existing `terminateWhenParentExits` is the backstop, not the plan). |
| LC-7 | Scene restore ordering | `releaseEditorsNow()` → destroy old strips → create new strips → `PluginProcessHost::start` in parallel → `adopt` in-process editor instances → publish. Children that fail to start fall back per-insert, never abort the load. |

### 5.3 HOW: Fallback — decide and write down

- If the child cannot start (missing exe, section/event creation
  failure, handshake timeout, `HOSTERROR`): run **in-process** exactly
  as today (`PluginHost::processBlock` on the live instance) and show
  a **visible warning** on the insert card ("Child host unavailable —
  running in-process"). Log the reason. Never silently mute, never
  silently degrade.
- Fallback is per-insert: one failed child does not force the rest
  in-process.
- Record the fallback reason in `PluginRack.log` with the `plugin: `
  prefix (existing convention).

### 5.4 HOW: Compatibility — decide and write down

| Topic | Rule |
| --- | --- |
| ASIO single-client | Child **never opens devices**. It receives buffers via shared memory. Test on the M-Audio (`Force ASIO` + duplex switch on) — the duplex callback path must be untouched. |
| Duplex mode | No change: `pullDuplexInput` / duplex buffers stay in the app. The child sees post-capture audio only. |
| 64-bit state sizes | State length is `uint64_t` on the wire; base64 cap documented (1 MiB suggest keeping, but as an explicit, logged refusal — not a silent `int` cast). |
| Sample rate / buffer changes | App-driven re-prepare (§5.1 IPC-8); `DeviceSettings` Apply path re-prepares children like it re-prepares instances today. |
| Scene restore | Save path asks the child for state with a **bounded timeout (500 ms)**; timeout → last-known-good state (see Phase 1 task 1.7). Restore path passes state on the `--plugin-host` command line (existing `-` placeholder convention). |
| Multi-class bundles | `pluginUniqueId` selects the class in **both** processes (child via CLI arg, in-process via `loadFromFile`). Mismatch = loud log + fallback, never silent wrong-plugin. |

### 5.5 HOW: Security — write this paragraph verbatim into the spec

> The helper runs with the user token; AppContainer isolation is out of
> scope. Process isolation (a faulting plugin kills its child, not the
> app) is the security benefit of this plan. `ensureAppContainer()`
> stays as profile preparation only. The existing trust warning stands:
> only load plugins from publishers you trust.

### 5.6 Exit criteria (all must be checked before Phase 1)

- [ ] Spec file exists at `docs/superpowers/specs/<date>-fault-containment-ipc.md`.
- [ ] Every IPC-1..10, LC-1..7 row has a decided value (not "TBD").
- [ ] Fallback (§5.3), compatibility (§5.4), security (§5.5) paragraphs present.
- [ ] Reviewer sign-off recorded in the spec (name + date).
- [ ] `ctest` green on the commit that adds the spec (docs-only change).

---

## 6. Phase 1 — Vertical slice (one insert, one plugin)

**Goal:** one insert renders through a child with a dry-bypass on
failure, behind a flag, with the in-process path still default.

### Task 1.1 — Introduce `PluginProcessHost` behind `PluginHost`

- **WHERE (new files):** `app/src/PluginProcessHost.h`,
  `app/src/PluginProcessHost.cpp`; register both in `app/CMakeLists.txt`
  (same `target_sources` block as `RestrictedHost.cpp`).
- **WHERE (call site):** `app/src/PluginHost.h` — add an
  `std::unique_ptr<PluginProcessHost> child_;` + `bool useChild_ = false;`
  (flag-controlled). `app/src/PluginHost.cpp::processBlock` — when
  `useChild_ && child_->isAlive()`, delegate; else today's path.
- **HOW:**
  1. Copy the `RestrictedHost` launch/watcher/exit-code skeleton (do not
     fork it — either subclass or move the shared half into a
     `ChildProcessBase`; the spec decides which, but **no duplicated
     watcher logic**).
  2. Implement exactly this surface (v1 API, kept):
     ```cpp
     bool start(const juce::File& path, int uniqueId, const juce::String& state);
     bool submitBlock(const juce::AudioBuffer<float>& scratch); // copy-only, never the live block
     bool fetchBlock(juce::AudioBuffer<float>& scratch, double& rate, int& block);
     void setParameter(uint32_t id, float value); // enqueue; Phase 2 wires the other end
     juce::String requestState(int timeoutMs = 500); // Phase 1: timeout → "" (caller uses last-known)
     bool isAlive() const; int exitCode() const; int missedBlocks() const;
     ```
  3. Reuse `PluginHostIpc.h`'s channel + `-req`/`-rsp` events; add any new
     header fields from the Phase 0 spec (peaks, latency) now, even if
     Phase 2 is what reads them.

- **Verify:** builds with `/W4 /WX`; `ctest` green; no behaviour change
  with the flag off (flag defaults off — see §9).

### Task 1.2 — Child renders (`runHost` revival)

- **WHERE:** `app/src/RestrictedHost.cpp::runHost` (extend the existing
  audio loop, do not write a second loop); `app/src/Main.cpp`
  `--plugin-host` arg parsing (add any new args the spec chose, keeping
  the `-` empty-state placeholder convention).
- **HOW:**
  1. Child creates the instance with `createPluginInstance` (same
     `disableNonMainBuses` + stereo-layout + `setRateAndBufferSizeDetails`
     sequence as `PluginHost::instantiate` — factor the shared steps
     into a helper both call sites use).
  2. Child loop: wait on `-req` → torn-read check → `plugin->processBlock`
     → write peaks + `responseSequence` → signal `-rsp` (mirror the
     existing loop; add the new header writes from the spec).
  3. Child answers the `{"prepare":{sr,bs}}` control line (see IPC-8) and
     the state/param side channel the spec chose.

- **Verify:** `--test-plugin-audio` against a known-good plugin renders
  non-silent audio through the child with the flag on (log shows the
  child path, not `in-process`).

### Task 1.3 — In-process instance becomes editor-only

- **WHERE:** `app/src/PluginHost.cpp::adopt` + `::processBlock`;
  `app/src/RackView.cpp` (no editor change — that is the point).
- **HOW:**
  1. When `useChild_` is true, `adopt` still creates/prepares the
     in-process instance (editor + params need it) but `processBlock`
     never calls `plugin->processBlock` on it — the child is the only DSP.
  2. Mirror the **initial state** (constructor `stateBase64`) into the
     child at `start()` time (command-line handover, existing convention).
  3. Editor parameter changes are captured (existing `PluginKnobWatcher`
     + generic param panel paths) and enqueued via `setParameter`; the
     child drains the queue per block (full two-way bridge is Phase 2;
     Phase 1 needs editor → child only, enough for the null test).

- **Verify:** with the flag on, opening the editor and turning a knob
  changes the rendered output (this is the audible-edit test the old
  `RestrictedHost` path failed).

### Task 1.4 — Per-block timeout + dry bypass (audio thread)

- **WHERE:** `app/src/PluginHost.cpp::processBlock` (delegate branch);
  `app/src/AudioEngine.cpp::render` insert loop (miss accounting +
  `LatencyReport`/`FifoDiagnostics` counters if the spec adds them).
- **HOW:**
  1. `processBlock` copies the live range into a **per-slot scratch**
     (`juce::AudioBuffer`, sized in `prepareToPlay` — never the live
     block, so a fallback leaves audio intact), `submitBlock(scratch)`,
     then `fetchBlock(scratch)` with the §5.1 IPC-3 deadline.
  2. On miss/timeout/torn-read: leave the live buffer untouched (dry),
     increment `missedBlocks`, log at most once per second (no log spam
     on the audio thread — use a `missLoggedMs` throttle like the
     existing underrun paths).
  3. After `N = 3` consecutive misses (LC-3): mark the insert failed,
     stop submitting (stay dry), notify the message thread via an atomic
     + `MessageManager::callAsync` (never a lock, never a dialog on the
     audio thread).
  4. **Do not double-process:** a block is rendered by the child or
     passed dry — never both, never the in-process instance as a
     "backup renderer" (that was the dead-meters bug).

- **Verify:** new diagnostic `--test-kill-child` (task 1.6) kills the
  child mid-render → app alive, insert dry, other strips rendering.

### Task 1.5 — `prepareToPlay` / sample-rate / buffer changes

- **WHERE:** `app/src/PluginHost.cpp::prepareToPlay`;
  `app/src/AudioEngine.cpp` settings-Apply path.
- **HOW:** when `useChild_`, `prepareToPlay(sr, bs)` resizes the scratch,
  sends `{"prepare":{sr,bs}}` to the child, and refreshes the latency
  cache from the child's reported value (or 0 until Phase 2 wires it).
  A child that does not ack a prepare within 500 ms → fallback warning
  (rule §4.1), not silence.

- **Verify:** change buffers in Settings → Apply with a child insert
  live → audio continues, log shows child re-prepare.

### Task 1.6 — Diagnostics: `--test-kill-child` + extended `--test-plugin-audio`

- **WHERE:** `app/src/Main.cpp` (next to `--test-plugin-audio`);
  allowlist in `moreThanOneInstanceAllowed()` (add the new switch so it
  runs as a short-lived child like the other `--` modes).
- **HOW:**
  1. `--test-plugin-audio <path> --use-child`: loads one plugin through
     the child with audio pumping (reuse the existing probe thread).
  2. `--test-kill-child <path> --kill-after-ms N`: as above, then
     `TerminateProcess(child, 0xC0000005)` after N ms and assert: app
     alive after 15 s, insert output == dry input (bit-compare a
     captured window), other strips still rendering, insert reports
     failure with exit code.

- **Verify:** both switches pass on a known-good plugin (e.g. Bertom
  VocalCompressor in repo root) and the kill test passes by construction
  on any plugin.

### Task 1.7 — Scene save through the child (bounded)

- **WHERE:** `app/src/PluginHost.cpp::getStateAsBase64`;
  `app/src/AudioEngine.cpp::makeScene` (must not hold `stateLock` across
  plugin/child calls — existing rule); `app/src/SceneBridge.cpp` if the
  codec needs the 64-bit length prefix.
- **HOW:** when `useChild_`, `getStateAsBase64` calls
  `requestState(500)`; on timeout/empty, return the **last-known-good**
  state cached at the last successful fetch (initialised from the
  constructor state). Log which path was taken.

- **Verify:** save a scene with a child insert alive → reload → settings
  match; kill the child → save → loads with last-known state + warning.

### Task 1.8 — Null test (in-process vs child bit-identity)

- **WHERE:** new `tests/test_plugin_process_null.cpp` (Catch2) + a
  `tools/render_cli` mode if offline rendering is easier than live.
- **HOW:** render a fixed input (sine sweep + impulses, 48 kHz / 256 and
  96 kHz / 128) through the same plugin in-process and through the
  child; compare output buffers sample-wise. Pass = bit-identical or
  within the spec's stated tolerance (some plugins dither — the spec
  must say which tolerance applies, not the test).

- **Verify:** null test passes for at least two real plugins (one
  bundled-VST-behaving, one M-Audio-scene plugin) before Phase 2 starts.
  **The child path does not become default until this passes (§4.5).**

### Phase 1 exit criteria

- [ ] Flag-off behaviour is today's behaviour (no child spawned, `ctest` green).
- [ ] Flag-on: kill-child test passes (app alive, insert dry, others render, failure reported).
- [ ] Flag-on: audible-edit test passes (knob turn changes child output).
- [ ] Flag-on: null test passes (child == in-process).
- [ ] Flag-on: scene save with live child returns plugin state; timeout falls back to last-known.
- [ ] No audio-thread alloc/block (source-invariant tests green).
- [ ] `/W4 /WX` clean.

---

## 7. Phase 2 — Editor, parameters and meters

**Goal:** the child insert is indistinguishable from in-process in
normal use: params both ways, live meters, state, latency.

### Task 2.1 — Two-way parameter bridge (coalesced per block)

- **WHERE:** `app/src/PluginHostIpc.h` (param ring layout from spec);
  `app/src/PluginHost.cpp` (`setParameter` enqueue + per-block drain);
  `app/src/RestrictedHost.cpp::runHost` (child-side drain + enqueue-back);
  `app/src/PluginKnobWatcher.*` + generic param panel (no redesign — they
  already observe the in-process instance; the bridge mirrors values).
- **HOW:**
  1. Editor → child: each `setParameter` enqueues `(id, value, seq)`;
     the audio thread drains the queue into the child's ring once per
     block (coalesce: last value per id wins).
  2. Child → editor: child enqueues automation/gesture changes the same
     way; the message thread applies them to the in-process params via
     `MessageManager::callAsync` coalesced at ≤ 30 Hz (never block the
     message thread on the child).
  3. On counter mismatch (`paramSeq` gap): resend full state (task 2.3)
     and log the divergence.

- **Verify:** automation recorded in the child appears in the editor;
  100 rapid knob turns do not grow the queue without bound.

### Task 2.2 — Meters: child peaks drive the strip

- **WHERE:** `app/src/PluginHostIpc.h` (header peak fields);
  `app/src/AudioEngine.h` (`InsertSlot` + `Channel::peakL/peakR`);
  `app/src/ChannelStrip.cpp` / `MixerComponent.cpp` (read side — should
  need no change if they already read the slot atomics).
- **HOW:** child writes `peakInL/R + peakOutL/R` per block; app copies
  them into the slot's atomics on `fetchBlock`; the strip meter reads
  those instead of the in-process tap. On miss, hold last value (do not
  zero-flicker).

- **Verify:** child insert meters move identically to in-process
  (side-by-side null-style check); killed child freezes the meter with
  the failure badge, not silence.

### Task 2.3 — State: save/restore through the child

- **WHERE:** `app/src/PluginHost.cpp::getStateAsBase64` (extend task 1.7);
  `app/src/AudioEngine.cpp::makeScene` / `restoreScene`;
  child `runHost` state handler.
- **HOW:** full-blob protocol from spec (IPC-5): `requestState` with
  `uint64_t` length, 500 ms timeout, last-known fallback; restore via
  CLI handover + `{"setState":…}` control for live re-push. Log
  divergences (child state hash ≠ editor state hash after a bridge
  cycle) and resend full state on mismatch.

- **Verify:** save → kill child → reload → state matches last-known;
  save → restore with live child → bit-identical blob round-trip.

### Task 2.4 — Latency: fold the child's report into compensation

- **WHERE:** child `runHost` (report `getLatencySamples()` after every
  prepare + on `{"getLatency"}`); `app/src/PluginHost.cpp::refreshLatencyCache`
  (read child value when `useChild_`); `app/src/AudioEngine.cpp`
  `LatencyReport::pluginMs` + existing compensation.
- **HOW:** header `latencySamples` field (IPC-7) is the fast path;
  control line is the audit path. `getLatencySamples()` stays lock-free
  (cached atomic, existing pattern).

- **Verify:** a latent plugin (e.g. Ozone-class linear-phase) reports
  the same latency through the child as in-process; budget view matches.

### Task 2.5 — Rate/buffer-change hardening

- **WHERE:** `app/src/AudioEngine.cpp` Apply/reload paths;
  `app/src/DeviceStream.*` untouched (child never opens devices).
- **HOW:** every `prepareToPlay` change re-prepares the child before the
  next block (IPC-8); a child that acks with a drifted `sr/bs` echo is
  re-prepared once more, then marked failed with a visible warning if it
  still drifts.

- **Verify:** Settings Apply across 44.1/48/96 kHz + 64–2048 frames with
  a live child insert → no stuck format, no silence.

### Phase 2 exit criteria

- [ ] Two-way params: edits + automation round-trip without message-thread stalls.
- [ ] Meters identical to in-process; killed child freezes + badges.
- [ ] State round-trips (live + killed-child fallback).
- [ ] Latency matches in-process for a latent plugin.
- [ ] Rate/buffer sweep clean.
- [ ] `ctest` green, `/W4 /WX` clean.

---

## 8. Phase 3 — Quarantine and UI

**Goal:** a crashed insert is named, dry, explainable, and reloadable —
and stays crashed until the user says otherwise.

### Task 3.1 — Session fault record (no re-spawn until Reload)

- **WHERE:** `app/src/AudioEngine.h` (`InsertSlot`: add
  `std::atomic<bool> failed{false}; int failExitCode{0}; juce::String failReason;`);
  `app/src/AudioEngine.cpp::render` (set on LC-4, clear on Reload only);
  `app/src/PluginProcessHost.*` (`killAndReap` + `exitCode`).
- **HOW:** on child death/wedge: `failed = true`, store exit code +
  plugin path, stop submitting, stay dry. `Reload` (task 3.3) is the
  only path that clears `failed` and re-spawns.

- **Verify:** kill test → `failed == true`, still `true` after 60 s with
  no respawn; Reload → `false`, audio returns.

### Task 3.2 — Persist quarantine in `faults.json`

- **WHERE (new file):** `%APPDATA%\PluginRack\plugins\faults.json`
  (written by the app, read on startup); code in `app/src/PluginLibrary.*`
  (next to the existing `blocked-plugin-loads.txt` logic) or a new
  `app/src/PluginFaults.*` — spec decides, but keep it beside the
  blocklist code.
- **HOW:**
  ```json
  { "faults": [
    { "path": "C:\\…\\Suspect.vst3", "exitCode": "0xC0000005",
      "time": "2026-11-13T10:24:00Z", "insertId": 7, "scene": "M Audio Duo ASIO 4ms" }
  ] }
  ```
  Append on every child death (bounded: keep last 100, prune on write).
  A faulted path loads **flagged but loadable** (unlike the load
  blocklist, which refuses) — the user chose Reload once, so the next
  scene load warns instead of refusing.

- **Verify:** kill → `faults.json` gains one entry; restart → entry
  present; Settings Clear wipes it (task 3.3).

### Task 3.3 — Strip UI: badge + Reload + Settings entry

- **WHERE:** `app/src/RackView.cpp` insert cards (`InsertSlotComponent`
  badge slot); `app/src/InsertMiniView.*` if the badge needs a shared
  widget; `app/src/SettingsView.cpp` (faults section with **Clear**
  button); `app/src/Theme.h` (badge colours — reuse the existing error
  red, do not invent a palette).
- **HOW:**
  1. Failed insert card shows `Plugin crashed (0xC0000005)` + **Reload**
     button; card keeps the plugin name (never "Unknown"); audio stays dry.
  2. Reload re-spawns the child with last-known state and clears `failed`
     only on `HOSTREADY`.
  3. Settings → Faults lists `faults.json` (path, exit, time) with Clear.

- **Verify:** kill → badge appears within 1 s, name intact, Reload
  restores audio; Clear empties the list.

### Task 3.4 — Docs + trust text update

- **WHERE:** `README.md` troubleshooting table (new row: "A plugin
  crashed mid-session"); `PRODUCT.md` if it promises crash behaviour;
  the in-app trust warning stays (Phase 0 §5.5).
- **HOW:** document: badge meaning, Reload, `faults.json` location,
  difference between load-blocklist (refuses) and fault quarantine
  (warns). One paragraph each, no new pages.

- **Verify:** docs build/lint (if any) green; troubleshooting row
  matches the actual badge text verbatim.

### Phase 3 exit criteria

- [ ] Killed child → named badge + dry audio + `faults.json` entry, no respawn.
- [ ] Reload restores; Settings Clear wipes.
- [ ] Docs match the UI text.
- [ ] `ctest` green, `/W4 /WX` clean.

---

## 9. Rollout & flags (v2 — new)

- **WHERE:** `AudioEngine::DeviceSettings` (new `bool childProcessDsp = false;`
  + `juce::StringArray childProcessOptOut;`) or a parallel
  `PluginFlags` struct — decide in Phase 0, but persist with the scene
  like the other tuning (same `std::optional` + `kSchemaVersion` rules).
- **HOW:**
  1. Phase 1–2: flag defaults **off**; enable per-insert via a
     right-click menu item ("Run in child process (beta)") or a
     `--use-child` test switch. Log which path each insert uses at
     `adopt` time (`in-process` vs `child`, mirroring today's
     `plugin: prepare … (in-process)` line).
  2. Per-plugin opt-out: if the null test or latency measurement shows a
     plugin is worse through IPC, add its path to the opt-out list
     (persisted); those inserts always run in-process with no warning
     (it is a measured decision, not a failure).
  3. Default-on is a **separate decision after Phase 2 exit criteria**,
     not part of this plan. Do not flip the default in a Phase 1–3 commit.
- **Verify:** flag off = zero children spawned (`rg "restricted host started"`
  absent from the log); flag on = one child per opted-in insert.

---

## 10. Risks (v1 table, kept + v2 owner/test)

| Risk | Mitigation | Owner | Test |
| --- | --- | --- | --- |
| Added block of latency and IPC overhead | Phase 1 null test; measure per-plugin and keep the in-process path for plugins that opt out | Phase 1 | Null test + per-plugin latency log |
| Editor and child state drift | Mirror parameters on every change, resend full state on mismatch, log divergences | Phase 2 | Bridge tests (task 2.1/2.3) |
| ASIO single-client / duplex | Route the child's device access through the same opener policy; test with the M-Audio | Phase 0 spec + Phase 1 | **Child never opens devices** (assert: no `DeviceStream` in child); M-Audio duplex repro |
| Child startup cost on scene load | Start in parallel with the other inserts, same as the device opener | Phase 1 (LC-1/LC-7) | Scene-load timing vs today (must not regress >10%) |
| False attribution when several plugins run | One child per insert in Phase 1; attribute by child, not by module | Phase 1 | Kill-child test names the right insert |
| Log spam / UI jank from per-block failures (v2) | Throttled logging (≤1 line/s per insert); message-thread notify via atomics + `callAsync` | Phase 1 (task 1.4) | Kill test: log lines bounded, UI responsive |
| Orphaned children on `TerminateProcess` shutdown (v2) | Reap owned children before terminating (LC-6); `terminateWhenParentExits` as backstop | Phase 1 | Shutdown with live children → no `PluginRack.exe` left in Task Manager |
| 64-bit state truncation (v2) | `uint64_t` length on the wire; explicit logged refusal over the cap | Phase 0 + Phase 2 | Oversize-state test |

---

## 11. Verification (v1, kept + v2 matrix)

v1 text, kept:

- Existing `--test-plugin-audio` can host a known-bad plugin; add a diagnostic
  that kills the child after N seconds and asserts the app still renders.
- Reproduce the 10 Nov `processAudio` family in the child and confirm the app
  survives with the insert bypassed.
- Full `ctest` stays green; the in-process path remains the default until the
  child path passes the null test.

v2 verification matrix (run in order; stop on first red):

| # | Command / action | Expect | Phase |
| --- | --- | --- | --- |
| V1 | `cmake --build build/dev --target pluginrack_app` | `/W4 /WX` clean | Every |
| V2 | `ctest --test-dir build/dev --output-on-failure` | Green | Every |
| V3 | `.\PluginRack.exe --test-load-save "M Audio Duo ASIO 4ms"` | 8.4 s-class load, `still alive`, no `crash-*.txt` | Every (Fix 1 guard) |
| V4 | `.\PluginRack.exe --test-plugin-audio "<good.vst3>" --use-child` | Renders, audible edits, live meters | 1–2 |
| V5 | `.\PluginRack.exe --test-kill-child "<any.vst3>" --kill-after-ms 2000` | App alive, insert dry, others render, badge + exit code | 1, 3 |
| V6 | Null test (`tests/test_plugin_process_null.cpp`) | Child == in-process (spec tolerance) | 1 (gate for 2) |
| V7 | Scene save (child alive) → reload | State round-trips | 1–2 |
| V8 | Scene save (child killed) → reload | Last-known state + warning | 1–2 |
| V9 | Settings Apply across rates/buffers with live child | No stuck format, no silence | 2 |
| V10 | M-Audio duplex repro (Force ASIO + duplex on) | Same latency budget shape as today; child never opens a device | 1–2 |
| V11 | Shutdown with live children | No orphaned `PluginRack.exe` | 1 |
| V12 | `faults.json` kill → restart → Clear | Entry persists, Clear wipes | 3 |

---

## 12. Out of scope (v1, kept)

- AppContainer/OS sandboxing.
- Cross-process editor embedding.
- Making the child the default for every plugin before Phase 1's null test.

v2 additions (explicit non-goals, to stop scope creep):

- Shared children (many inserts per child) — revisit after Phase 3 if
  process count becomes a problem; never in Phase 1.
- Network/UNC plugin paths — still refused (`isRemotePath`), in both processes.
- macOS/Linux ports — the IPC is Win32 (`NamedSection`/`NamedEvent`);
  portability is a separate plan.

---

## Appendix A — Exact code touchpoints (v2)

| Symbol | File | What happens to it |
| --- | --- | --- |
| `PluginHost::processBlock` | `app/src/PluginHost.cpp` | Gains the `useChild_` delegate branch (task 1.1/1.4); today it is the AV site |
| `PluginHost::adopt` / `prepareToPlay` / `getStateAsBase64` / `refreshLatencyCache` | `app/src/PluginHost.*` | Editor-only mode, child prepare, child state, child latency (tasks 1.3/1.5/1.7, 2.3/2.4) |
| `SharedAudioBlock` + `sharedNameFor` + event helpers | `app/src/PluginHostIpc.h` | New header fields per spec (peaks, latency, param/state seqs) |
| `RestrictedHost::launch/submitAudioBlock/fetchAudioBlock/runHost` | `app/src/RestrictedHost.*` | Revived as the DSP host; watcher pattern reused for death detection |
| `AudioEngine::render` insert loop | `app/src/AudioEngine.cpp` | Miss accounting, dry-bypass, `failed` flag |
| `AudioEngine::restoreScene/makeScene/addPluginAsync` | `app/src/AudioEngine.cpp` | Parallel child start, bounded child state save |
| `RackView::releaseEditorsNow/destroyEditorWhenIdle` | `app/src/RackView.*` | Unchanged by this plan (Fix 1 stays); badge UI added in Phase 3 |
| `--plugin-host/--test-plugin-audio` + new `--test-kill-child/--use-child` | `app/src/Main.cpp` | Child entry + diagnostics |
| `faults.json` read/write | `app/src/PluginLibrary.*` or new `PluginFaults.*` | Phase 3 |
| Insert badge + Settings faults section | `app/src/RackView.cpp`, `SettingsView.cpp`, `Theme.h` | Phase 3 |

## Appendix B — IPC header sketch (v2, illustrative — spec decides)

```cpp
// Extends app/src/PluginHostIpc.h — field names must match the Phase 0 spec.
struct SharedAudioBlock {
  // ... existing sequence/responseSequence/blockSequence guards ...
  int numFrames = 0; int bufferSize = 0; double sampleRate = 0.0;
  float interleave[kMaxBlockFrames * 2] = {};
  // --- Phase 1/2 additions (spec decides exact layout) ---
  float peakInL = 0, peakInR = 0, peakOutL = 0, peakOutR = 0; // child → app, per block
  int32_t latencySamples = 0;                                 // child → app, per prepare
  // Param ring (SPSC, 64 slots, even/odd seq discipline like the audio block):
  // struct ParamMsg { uint32_t id; float value; uint64_t seq; } paramRing[64];
};
```

## Appendix C — `PluginProcessHost` header sketch (v2, illustrative)

```cpp
// New: app/src/PluginProcessHost.h — behind PluginHost (task 1.1).
class PluginProcessHost {
public:
  bool start(const juce::File& path, int uniqueId, const juce::String& state);
  bool submitBlock(const juce::AudioBuffer<float>& scratch); // audio thread, non-blocking
  bool fetchBlock(juce::AudioBuffer<float>& scratch, double& rate, int& block); // bounded wait
  void setParameter(uint32_t id, float value); // any thread, enqueue only
  juce::String requestState(int timeoutMs = 500); // message thread, bounded
  bool isAlive() const; bool isDead() const; int exitCode() const;
  int missedBlocks() const; void killAndReap();
private:
  // Reuses pluginHostIpc::NamedSection + NamedEvent (-req/-rsp),
  // watcher-thread death detection, per-slot scratch — see RestrictedHost.
};
```

## Appendix D — Command cheat sheet (v2)

```powershell
# Build + test (every phase, every commit):
cmake -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DJUCE_DIR="C:/path/to/JUCE" -DVST3_SDK_DIR="C:/path/to/vst3sdk"
cmake --build build/dev --target pluginrack_app
ctest --test-dir build/dev --output-on-failure

# Fix-1 guard (must stay green while this plan sleeps):
.\PluginRack.exe --test-load-save "M Audio Duo ASIO 4ms"

# Phase 1 diagnostics (new in this plan):
.\PluginRack.exe --test-plugin-audio "C:\path\to\Good.vst3" --use-child
.\PluginRack.exe --test-kill-child "C:\path\to\Any.vst3" --kill-after-ms 2000

# Triage:
dir $env:APPDATA\PluginRack\logs\crash-*.txt
type $env:APPDATA\PluginRack\plugins\faults.json   # Phase 3 only
.\PluginRack.exe --clear-blocked-plugins
llvm-symbolizer --obj=PluginRack.exe 0x<address>
```
