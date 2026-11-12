# Fault containment IPC — Phase 0 spec

Status: **proposed, awaiting sign-off.** Phase 0 of
`docs/superpowers/plans/2026-11-13-plugin-fault-containment.md`. This document
decides the contract; it contains no implementation plan beyond what Phase 1
needs to start, and no code.

Parent plan: `docs/superpowers/plans/2026-11-13-plugin-fault-containment.md`
(trigger: a plugin faults `PluginHost::processBlock` on the audio thread;
Fix 1 `5132a41` removed the crash that motivated the plan, so this spec is
preparation, not immediate work).

## 1. Problem

A plugin that access-violates inside `IAudioProcessor::processBlock` kills the
whole process. `PluginHost::processBlock` (`app/src/PluginHost.cpp:664`) calls
`plugin->processBlock` directly, so the fault takes the app, every strip and
the unsaved scene with it. Nothing in-process can catch it: the only reliable
containment is to run the DSP in a process that is allowed to die.

## 2. Goals and non-goals

**Goals**

1. One insert renders in a child process; its fault costs that insert only.
2. A dead or wedged child leaves the insert **dry**, named and explainably
   failed — never silent, never a second renderer.
3. The in-process instance keeps owning the editor, parameters and meters'
   geometry, so the rack UI does not change in Phase 1.
4. The in-process path stays the default until the Phase 1 null test passes.
5. The child never opens an audio device (ASIO is single-client on the
   M-Audio rig).

**Non-goals**

- OS sandboxing / AppContainer launch (§5).
- Cross-process editor embedding.
- Shared children (many inserts per child).
- Network/UNC plugin paths, non-Windows ports.

## 3. Design

### 3.1 Decisions at a glance

Every row is decided. `Plan` is the row id in the parent plan (§5.1, §5.2).

#### IPC contract

| # | Decision | Value |
| --- | --- | --- |
| IPC-1 | Block format | Keep float32 stereo interleaved in `SharedAudioBlock::interleave` (`kMaxBlockFrames * 2`). Mono plugins are folded to/from a duplicated right channel at the edges, as today. |
| IPC-2 | Max frames | Keep `kMaxBlockFrames = 8192`. `submitAudioBlock` keeps clamping with `jmin` (it already does at `RestrictedHost.cpp:268`) and logs a throttled warning when it clamps; it never drops a block for being large. |
| IPC-3 | Per-block deadline | `deadlineMs = clamp(0.5 * bufferSize / sampleRate, 1 ms, 20 ms)`, computed in `PluginHost` at prepare time and passed to `fetchAudioBlock` as a parameter. Today the wait is a hardcoded `20.0` ms (`RestrictedHost.cpp:310`); that is the ceiling, not the rule. A miss = dry bypass for that block. |
| IPC-4 | Param messages | Two SPSC rings inside a dedicated `ParamSection` (§3.3), one per direction, 512 entries each. Entry = `{ uint32 id; float value; uint32 flags; uint32 pad; uint64 seq; }` (24 bytes, `ParamMsg`). Coalescing happens on the **producer** side in a preallocated 4096-entry open-addressing table (id → latest value, dirty bit); the ring therefore carries at most one message per changed id per flush, so 512 entries cannot overflow at human edit rates. Consumer applies messages in order, last-wins. |
| IPC-5 | State transfer | Full blob, in the `ControlSection` (§3.4): `uint64 payloadBytes` + base64 text. Cap stays `kMaxPluginStateBase64Chars` (1 MiB, `PluginHost.cpp:346`). Over the cap is an explicit, logged refusal that stores nothing new — never a truncation, never an `int` cast. |
| IPC-6 | Meter messages | Four floats in the audio header, written by the child **before** it bumps `responseSequence`: `peakInL, peakInR, peakOutL, peakOutR`. Unsigned block peak, no ballistics (the strip keeps its own). |
| IPC-7 | Latency report | `int32 latencySamples` in the audio header, refreshed after every prepare and every state load; `{"getLatency"}` control op is the audit path. Read on the message thread into `PluginHost`'s existing atomic cache (`cachedLatencySamples_`). |
| IPC-8 | Rate/buffer changes | App initiates. Spawn carries the initial `sampleRate`/`blockSize` (§3.5), so the child instantiates at the app's format and needs no early re-prepare. Later changes: `{"prepare":{"sampleRate":…,"blockSize":…}}` control op, acked before the next block; the child echoes both in every audio header so drift is detectable. |
| IPC-9 | Sequence discipline | Audio block keeps the existing `blockSequence` even/odd + `sequence`/`responseSequence` handshake untouched. Param and control rings use plain monotonic head/tail `uint64` atomics with release on publish / acquire on read; no even/odd marker, because only the producer writes a slot and only after claiming it (§3.3). |
| IPC-10 | Wake-ups | Four auto-reset events, derived from the channel name exactly like today: `-req` / `-rsp` (audio, unchanged) and `-ctl-req` / `-ctl-rsp` (control). Param traffic does **not** get its own event: the app flushes its param ring immediately before publishing a block, and the child flushes its ring before acking one. |

#### Lifecycle

| # | Decision | Value |
| --- | --- | --- |
| LC-1 | Spawn | One child per insert, `PluginRack.exe --plugin-host <file> <uniqueId> <mapping> <state\|-> <sampleRate> <blockSize>`. Spawned **and** handshaken on a worker, never on the message thread. For an interactive add that worker already exists: the `juce::Thread::launch` inside `addPluginAsync` (`AudioEngine.cpp:1189`), which today only scans. Scene restore has no worker today — `restoreScene` loads inserts serially on the message thread (`AudioEngine.cpp:2256`, `:2336`) — so Phase 1 adds the parallel spawn step there. Spawning is OS work only; the app-side editor instance still has to be built on the message thread. |
| LC-2 | Handshake | The child publishes `ControlSection::hostReady` (`0` starting, `1` loaded, `2` failed) and the app polls that flag with a 10 s budget. Stdout still prints `HOSTREADY <path>` / `HOSTERROR <reason>` for the log, but it is **not** the handshake: `ChildProcess::readProcessOutput` blocks when the pipe has no data, so polling stdout deadlocks the worker that is waiting. A ready child has every channel open and the plugin instantiated. On failure the app reads stdout only after the child has exited (`outputAfterExit()`), where a read returns at EOF. |
| LC-3 | Per-block timeout | IPC-3 deadline. Consecutive misses counted per insert; 3 in a row marks the insert failed (dry, badge), 1 miss is just a dry block. Any successful block resets the counter. |
| LC-4 | Child crash | Existing watcher (100 ms poll of `ChildProcess::isRunning`) sets `hostDead`; the audio thread only ever reads that atomic. The exit code and `pendingOutput()` are read on the message thread when the badge is built. |
| LC-5 | Restart policy | No auto-restart. `killAndReap()`, record the fault (Phase 3), re-spawn only on an explicit **Reload**. |
| LC-6 | App shutdown | `PluginRackApplication::shutdown()` (`app/src/Main.cpp:574`) must save the scene, **then** kill every child, **then** `TerminateProcess`. Order matters: `saveSceneOnExit()` asks children for state, so killing first would save "last-known-good" (or nothing) every exit. `AudioEngine` gains a public `shutdownChildHosts()` for this; `terminateWhenParentExits()` stays as the backstop for crashes. |
| LC-7 | Scene restore ordering | Keep `MainComponent.cpp:864-868`'s `releaseEditorsNow()` → `restoreScene` order. Per insert, Phase 1 splits today's serial `loadPlugin` into: (1) worker spawns child, waits `HOSTREADY`; (2) message thread instantiates + `adopt`s the in-process editor instance; (3) insert published with a fixed renderer (§3.2). A failed child falls back per insert and never aborts the load. |

### 3.2 Renderer identity rule (the one that must not be misread)

The parent plan says both "if the child cannot start, run in-process" (§5.3)
and "never use the in-process instance as a backup renderer" (§4.1, task
1.4.4). These are consistent only under this rule:

> **An insert's renderer is fixed when the insert is published.** A slot is
> either in-process from birth or child-backed from birth. Within a session the
> renderer changes only through Reload, which tears the slot down and rebuilds
> both sides.

- **Launch failure** (`HOSTERROR`, missing exe, section/event creation,
  10 s timeout): the slot is published **in-process**, with a visible
  "Child host unavailable — running in-process" warning and a log line. The
  editor, the parameters and the audio are then all the same instance, so
  meters and edits stay honest.
- **Runtime death or wedge** (LC-3, LC-4): the slot goes **dry** until Reload.
  It does not switch to the in-process instance, because that instance has been
  receiving no input for the whole session — its meters are stale, its
  parameters were mirrored away, and its filters carry the wrong history.
  Switching to it mid-session would be a silent, audible discontinuity, which
  is exactly the failure the old `RestrictedHost` wiring produced.

Two comments in the tree currently document the opposite and must be rewritten
in the same commit as task 1.4, or the next reader will re-implement the old
bug:

- `app/src/PluginHostIpc.h:15-24` — "the caller … falls back to rendering the
  in-process instance".
- `app/src/PluginHost.cpp:674-677` — the "this is the instance that renders"
  note gains the child branch's own caveat.

### 3.3 Param ring (`ParamSection`)

```cpp
namespace pluginHostIpc {

constexpr int kParamRingSlots = 512; // power of two
constexpr int kParamCoalesceEntries = 4096; // open-addressing, producer side

struct ParamMsg {
    std::uint32_t id = 0;
    float value = 0.0f;
    std::uint32_t flags = 0; // bit 0 = gesture/live edit, bit 1 = end of gesture
    std::uint32_t reserved = 0;
    std::uint64_t seq = 0;
};

struct ParamRing {
    std::uint64_t head = 0;             // producer writes, release
    std::uint64_t tail = 0;             // consumer writes, release
    std::uint64_t producerSeq = 0;      // diagnostics only
    std::uint64_t consumerSeq = 0;
    ParamMsg slots[kParamRingSlots];
};

struct ParamSection {
    ParamRing toHost;   // editor → child
    ParamRing toApp;    // child → editor (Phase 2)
};

} // namespace pluginHostIpc
```

- Producer coalesces in its own table, then publishes at most one message per
  changed id per flush. A knob drag at 60 Hz with 8 dirty ids costs 8 messages
  per block, not 60 per second.
- Ring full (should be impossible after coalescing): the producer **drops the
  message and increments `producerSeq` past the gap**, so the consumer sees a
  gap and requests a full state resync (Phase 2, task 2.1.3). A silent drop is
  not allowed.
- `ParamSection` is created by the app in `PluginProcessHost::start` and opened
  by the child right after the audio section, before the plugin is scanned —
  same reason as `RestrictedHost.cpp:364`: fail before spending seconds loading
  a plugin whose parameters can never be delivered.

### 3.4 Control channel (`ControlSection`)

`RestrictedHost::sendControl` is a **stub**: it ignores its arguments and
returns `{}` (`app/src/RestrictedHost.cpp:244-252`), because JUCE's
`ChildProcess` exposes no stdin pipe. IPC-5/7/8 therefore cannot use it, and
neither can the initial state handover. Decision: a second named section plus
two events, reusing `NamedSection`/`NamedEvent` as they already work
cross-process.

```cpp
namespace pluginHostIpc {

constexpr std::size_t kControlPayloadBytes = 1024 * 1024 + 4096; // base64 cap + header slack

struct ControlSection {
    std::uint64_t blockSequence = 0; // even = stable, odd = writer active
    std::uint64_t requestSeq = 0;    // app → child
    std::uint64_t responseSeq = 0;   // child echoes the requestSeq it answered
    std::uint64_t payloadBytes = 0;  // valid length of `payload`
    std::uint32_t op = 0;            // see ControlOp
    std::uint32_t status = 0;        // 0 = ok, 1 = refused, 2 = failed
    char payload[kControlPayloadBytes] = {}; // JSON line, NUL-terminated
};

enum class ControlOp : std::uint32_t {
    prepare = 1,      // {"prepare":{"sampleRate":48000,"blockSize":256}}
    setState = 2,     // {"setState":{"base64":"…"}}
    getState = 3,     // → {"ok":true,"base64":"…"}
    getLatency = 4,   // → {"ok":true,"latencySamples":N}
    ping = 5          // liveness / watchdog probe
};

} // namespace pluginHostIpc
```

- Request/response is strictly one in flight: the app writes the section, bumps
  `requestSeq`, signals `-ctl-req`, waits on `-ctl-rsp` for a bounded budget,
  and reads the reply only when `responseSeq == requestSeq`.
- Budgets are per op, never unbounded: `prepare` 500 ms, `setState` 500 ms,
  `getState` 500 ms (the parent plan's number), `getLatency` 100 ms, `ping`
  100 ms. A timeout is a normal result the caller handles (§3.7), not an
  assertion.
- The control channel is message-thread / worker-thread only. The audio thread
  never touches `ControlSection`, which is why its 1 MiB payload cannot hurt
  the audio path's cache behaviour.
- Section size is fixed at creation, so `NamedSection::create` is called with
  `sizeof(ControlSection)` (~1.05 MB) — a pagefile-backed section, committed
  lazily, so the idle cost is the header page plus whatever the plugin's state
  actually touches.

### 3.5 Spawn and handshake

Command line (positions are fixed; unknown extra args are ignored by older
builds):

```
PluginRack.exe --plugin-host <pluginPath> <uniqueId> <mappingName> <state|-> <sampleRate> <blockSize>
```

- **Initial state never travels on the command line.** Windows caps a command
  line at 32767 characters; base64 state may legitimately be 1 MiB. The `-`
  placeholder stays (it keeps positions fixed, `RestrictedHost.cpp:161-168`),
  and the app pushes the real state as the first `setState` control op after
  `HOSTREADY`.
- `<sampleRate>`/`<blockSize>` are additive positions 5 and 6; the child
  instantiates at those values instead of the current hardcoded `48000/256`
  (`RestrictedHost.cpp:414`). A missing pair keeps the old default, so an old
  binary still works against a new app.
- Handshake on stdout, unchanged in spelling: `HOSTREADY <path>` or
  `HOSTERROR <reason>`, 10 s budget. The child must print `HOSTREADY` only
  after the plugin instance exists **and** the param and control sections are
  open, so a ready child is a fully wired child.
- Handshake, corrected during Phase 1: readiness is the shared flag
  `ControlSection::hostReady`, not the stdout text. `HOSTREADY <path>` and
  `HOSTERROR <reason>` are still printed (the log and the failure report use
  them), but polling the output pipe blocks while it is empty and hung the
  loading worker — that is exactly the kind of bug a "HOSTREADY on stdout"
  contract invites, so the flag is authoritative.
- The child stores its own pid in `ControlSection::hostPid` before loading
  anything: this JUCE `ChildProcess` exposes no pid, and a fault record (and the
  `--test-child-host --kill-after-ms` diagnostic) has to name the process.
- `--test-child-host` and any `--use-child`-style switch must be added to
  `moreThanOneInstanceAllowed()` (`app/src/Main.cpp:196-203`), or the second
  launch takes the single-instance path and never runs the diagnostic.

### 3.6 Child input policy

The child renders buffers it is handed and touches no device API. Concretely,
the child must not link behaviour to `setChannelInputDevice`,
`setOutputDevice`, `AudioDeviceManager`, `DeviceStream`, or
`JUCEApplicationBase::getCommandLineParameterArray` beyond argument parsing.
This is an invariant test, not a review note (§4, T3).

### 3.7 Fallback and failure taxonomy

| Situation | Renderer after the event | UI | Log |
| --- | --- | --- | --- |
| Child starts, handshakes | child | none | `plugin: child host ready for <name>` |
| `HOSTERROR`, missing exe, section/event failure, 10 s timeout | in-process (decided at publish time, §3.2) | card warning "Child host unavailable — running in-process" | reason with `plugin: ` prefix |
| One block misses its deadline | child, block plays dry | none (transient) | throttled ≤ 1 line/s |
| 3 consecutive misses | dry until Reload | "Plugin not responding" + Reload | one line |
| Child exits (any code) | dry until Reload | "Plugin crashed (0x…)" + Reload | exit code + last stdout |
| Control op times out (`getState` at save) | unchanged | none | last-known-good state used |

Never: silence without a reason, a second renderer, or a spawn/retry loop.

### 3.8 Prepare lifecycle must match the in-process path

`PluginHost::instantiate` prepares once, and `PluginHost::adopt` prepares again
on the message thread. Plugins are allowed to reseed internal state in
`prepareToPlay`, so the child must receive **two** prepares for the same load:

1. the one inside `PluginHost::instantiate`, on the child's own thread, and
2. the `prepare` control op the app sends from `loadChildHost`.

The child therefore re-prepares on every `prepare` request, even when the
requested rate and block size already match what it was instantiated with. The
first version skipped that prepare when the format matched, and the null test
(`--test-child-null`) caught the consequence: Bertom VocalCompressor rendered
bit-identically in-process against itself, but differed on **every sample**
through the child with a max difference of 0.888. Making the child prepare
twice produced a bit-identical result.

Rule for Phase 2 onwards: any lifecycle call the app makes against its
in-process instance and that can change plugin state must have a matching call
on the child, in the same order. `releaseResources` is not exempt.

### 3.9 Meters and latency plumbing

- `InsertSlot` (`app/src/AudioEngine.h:36`) gains
  `float peakInL/peakInR/peakOutL/peakOutR` as `juce::Atomic<float>` and
  `int reportedLatencySamples`. Channel meters today live on `Channel`
  (`AudioEngine.h:96`); the strip reads the slot values when the slot is
  child-backed and the channel values otherwise. Phase 1 only writes the
  fields; Phase 2 reads them.
- Latency: the child's `latencySamples` header field is copied into
  `PluginHost::cachedLatencySamples_` on the message thread (never on the audio
  thread), so `getLatencySamples()` stays lock-free as documented at
  `PluginHost.h:103-105`.

## 4. Testing

Phase 0 lands as a docs-only commit. The tests below are what Phase 1 must add
or extend; the parent plan's verification matrix (V1–V12) stays authoritative.

| # | Test | Where |
| --- | --- | --- |
| T1 | Param ring SPSC correctness: N producer flushes, one consumer, no loss/duplication, coalescing keeps last value | `tests/` — the ring is JUCE-free, so the core test binary can cover it |
| T2 | Control section round trip: op/response pairing, one-in-flight enforcement, timeout returns a status and leaves the section consistent | `tests/` |
| T3 | Source invariant: no `DeviceStream`/`AudioDeviceManager`/`setOutputDevice` in the child translation units | `tests/test_underrun_paths.cpp`-style source scan |
| T4 | Null test, in-process vs child (parent plan task 1.8) | `--test-child-null <path>`: renders 200 blocks through an in-process instance, a second in-process instance (determinism baseline) and the child, then compares. First run found the prepare-lifecycle bug in §3.8. |
| T5 | Kill test: app alive, insert dry, others render, exit code recorded | `--test-child-host <path> --kill-after-ms N` |

Tolerance for T4 (decision, so the test does not invent one): **bit-identical
for the first 1000 blocks**; if a plugin dithers, its tolerance must be
recorded per plugin path in the spec's Phase 1 results, never relaxed globally.
The diagnostic prints a determinism baseline (a second in-process instance run
on the same input) so a plugin that differs against itself is visible before its
child difference is called a transport fault.

Phase 1 results (48 kHz / 256, 200 blocks): `Scope.vst3`,
`AB Reference 2.vst3`, `GBCAudio SpectraVU.vst3`, `Bertom_DenoiserClassic.vst3`,
`Bertom_VocalCompressor.vst3`, `True Panner.vst3` and `Ozone Imager 2.vst3` all
bit-identical, baseline 0. See §7.1 for what is still blocking the engine
wiring.

## 5. Compatibility

| Topic | Rule |
| --- | --- |
| ASIO single-client | Child never opens a device (§3.6). Verified on the M-Audio with `Force ASIO` + duplex on; the duplex callback path in the app is untouched. |
| Duplex | `pullDuplexInput` and duplex buffers stay in the app; the child sees post-capture audio only. |
| 64-bit state sizes | `uint64_t` on the wire; 1 MiB base64 cap retained as an explicit, logged refusal (`PluginHost.cpp:346`, `:567`). |
| Rate/buffer changes | Spawn carries the format; later changes use `prepare` (IPC-8), which always re-prepares the child rather than skipping a matching format (§3.8); a drifted echo gets one re-prepare, then the slot is marked failed. |
| Scene restore | Save asks the child with a 500 ms budget; timeout → last-known-good state, logged. Restore pushes state via `setState`, not argv (§3.5). |
| Multi-class bundles | `pluginUniqueId` selects the class in both processes (`--plugin-host` arg 2, `loadFromFile` in-process). A mismatch is a loud log plus fallback, never a silently different plugin. |
| Scene schema | Adding `DeviceSettings` flags bumps `kSchemaVersion` 7 → 8 (`core/src/SceneCodec.cpp:9`); missing fields read as `false`, and old files must load with the feature off. |
| Second launch | Any new diagnostic switch goes in `moreThanOneInstanceAllowed()` (§3.5). |

## 6. Security

> The helper runs with the user token; AppContainer isolation is out of
> scope. Process isolation (a faulting plugin kills its child, not the app) is
> the security benefit of this plan. `ensureAppContainer()` stays as profile
> preparation only. The existing trust warning stands: only load plugins from
> publishers you trust.

This is the code's actual behaviour, not an aspiration: `RestrictedHost.cpp:88-95`
logs exactly that the child runs with the normal user token, and
`CreateAppContainerProfile` only creates the profile. The spec must not be read
as claiming a sandbox, and no UI text may imply one.

## 7. Risks introduced by this spec

| Risk | Mitigation |
| --- | --- |
| Control channel does not exist today (`sendControl` is a stub) and Phase 1 assumes it | §3.4 adds it explicitly as Task 1.0, before any audio work |
| 1 MiB control section per child | Sections are pagefile-backed and committed lazily; one per insert, and the cap is already the state cap |
| Command-line size limit silently truncating state | State never travels on the command line (§3.5) |
| Renderer flipping mid-session | §3.2 fixes identity at publish time; Reload is the only transition |
| Two stale comments describing the old fallback | Rewritten in the same commit as task 1.4 (§3.2) |
| Param coalescing table (4096 entries) cost on the message thread | It is only touched on edit/automation traffic, never per block, and never on the audio thread |

## 7.1 Phase 1 status (recorded 13 Nov 2026)

**Landed and verified** (`e8c7c11`, `b673010` plus the fixes below):

- `PluginIpcLayout.h`: param ring and control section, JUCE-free, covered by
  the core test binary.
- `PluginProcessHost`: spawn, handshake, bounded fetch, control ops
  (prepare/setState/getState/getLatency/ping), watcher, reap.
- Child loop: one loop, two wake-ups (audio + control), per-block params,
  peaks and latency in the audio header.
- `PluginHost` child branch: dry on death/miss, three consecutive misses mark
  the slot failed, state save asks the child then falls back to last-known.
- Diagnostics: `--test-child-host` (render), `--test-child-host --kill-after-ms`
  (kill/attribution), `--test-child-null` (A/B vs in-process with a determinism
  baseline).

Verified results: null test bit-identical on VocalCompressor (after §3.8),
Denoiser, Scope, AB Reference 2, SpectraVU, True Panner and Ozone Imager 2;
kill test reports dry output, exit `0xC0000005`, app alive; 368/368 ctest;
`--test-load-save "M Audio Duo ASIO 4ms"` unchanged.

**Bugs found by the diagnostics, both fixed:**

1. Prepare lifecycle mismatch (§3.8) — a compressor that reseeds in
   `prepareToPlay` rendered differently through the child.
2. `submitBlock` sent the whole preallocated scratch as the block length, so
   every child block played dry once the scratch became 8192 frames. The frame
   count is now explicit.

**Blocker (this is why the engine wiring is not in the tree):** wiring the
child to live inserts under real device audio crashed 4 of 8 twenty-second
soak runs, with two signatures — `0xC0000005` and heap corruption
`0xC0000374` — while the fixed-format null test (48 kHz / 256, 200 blocks)
passed and a plain run without children survived. A PageHeap run put the fault
inside `VST3PluginInstanceHeadless::processAudio` via `PluginHost::processBlock`
on the audio thread, which for a corruption report is the *detection* site, not
necessarily the write site.

The wiring (AudioEngine flag and spawn, `--use-child`, knob forwarding, shutdown
reap) was implemented, verified partially, then **reverted** because a switch
that crashes is worse than no switch. Suspects to audit before it returns:

- `childHost_` lifetime versus the audio thread: `stopChildHost()` resets the
  pointer while `processBlock` may be inside it (use-after-free).
- `prepareToPlay` can run on the audio thread at device start and calls the
  control channel, which is documented as one-in-flight and message-thread
  only; `refreshLatencyCache()`/`getStateAsBase64()` use it from the message
  thread.
- Any allocation reachable from `prepareToPlay` (the constructor-allocation fix
  removed one; the audit must prove there are no others).

Rule until that audit passes and a 30-run soak is clean: no `--use-child`-style
switch, no engine wiring, and the child path stays reachable from diagnostics
only.

## 8. Out of scope

- AppContainer launch, low-integrity tokens.
- Cross-process editors.
- Shared children, and any change to `ChannelStrip`/`MixerComponent` beyond
  reading the slot's peaks (Phase 2).
- macOS/Linux: `NamedSection`/`NamedEvent` are Win32.

## 9. Sign-off

- Reviewer: _pending_
- Date: _pending_
- Phase 1 may not start until both are filled in and the parent plan's §5.6
  checklist is fully checked.
