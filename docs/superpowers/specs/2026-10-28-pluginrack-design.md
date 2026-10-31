# PluginRack — Design Specification

**Date:** 2026-10-28
**Status:** Approved (Approach A)
**Scope:** Windows-first MVP, architected for later macOS/Linux

## 1. Problem

Voicemeeter-style routing forces users onto a fixed set of built-in effects and a
fixed channel layout. PluginRack keeps the routing model but lets every input
carry the user's own VST plugins, and keeps the input count open-ended.

A user should be able to:

1. Bring audio in from hardware devices, system playback, and other applications.
2. Insert their own plugins, in order, on each input.
3. Mix with familiar channel controls (gain, pan, mute, solo, metering).
4. Send each channel to any combination of physical and virtual outputs at
   independent levels.

## 2. Product Scope (MVP)

### In scope

| Area | MVP requirement |
| --- | --- |
| Platform | Windows 10/11 x64 desktop application |
| Sources | Hardware capture devices, system loopback, virtual input endpoints |
| Channel layouts | Mono and stereo channels; mono is upmixed to stereo for processing |
| Plugin formats | VST3 required; VST2 best-effort behind the same host interface |
| Plugin hosting | In-process, ordered insert chain per channel |
| Mixer controls | Gain, pan, mute, solo, peak metering |
| Routing | Many-to-many matrix, independent post-fader send level per channel→output cell |
| Destinations | Physical devices and virtual output endpoints |
| Clock | One engine-wide sample rate and buffer size, negotiated against devices |
| Latency | Live-monitoring quality with selectable buffer sizes |
| Persistence | Named scenes, saved and loaded as JSON |
| Failure policy | Plugin failure → bypass that plugin; source failure → stop that source |
| Layout | Scrollable channel strips plus a separate routing matrix view |

### Out of scope for MVP

- macOS and Linux builds (architecture must not preclude them)
- Submix buses, nested routing, pre-fader sends
- Per-device sample-rate conversion
- Arbitrary multichannel layouts (5.1/7.1)
- Out-of-process plugin sandboxing
- File, network, and plugin-generated sources
- Recording/export destinations inside the mixer (the offline render tool is a
  development harness, not a product feature)

## 3. Architecture

### 3.1 Three subsystems

1. **Core engine (`core/`)** — platform-neutral C++20. Scene model, validation,
   routing math, insert chains, metering, WAV output. No JUCE, no OS calls.
   This is the part that must be identical on every future platform, and the
   part that is fully testable without audio hardware.
2. **Windows host (`app/`, `platform/win/`)** — JUCE application shell, WASAPI
   device I/O, system loopback capture, VST3/VST2 hosting, virtual audio
   endpoints, installer and driver signing.
3. **UI (`ui/`)** — JUCE components: channel strips, insert chain editor,
   routing matrix, scene browser.

The core engine is deliberately JUCE-free so that the routing rules, scene
format, and failure semantics can be tested in milliseconds on any machine. JUCE
appears only where a device, plugin, or window is involved.

### 3.2 Data flow

```
source capture ──► channel input ──► insert chain ──► gain ──► pan ──► mute/solo
                                                                          │
                                                                          ▼
                                                              routing matrix (send gain)
                                                                          │
                                                                          ▼
                                                    output bus ──► device / virtual endpoint
```

Processing is planar float32. A channel is stereo internally. A mono source is
duplicated to both sides before the insert chain, so plugins always see stereo.

### 3.3 Threading

- **Audio thread:** `MixerEngine::render` only. No allocation, no locks, no
  logging, no file or OS access.
- **Control thread:** scene edits, plugin loading, device changes. Publishes
  parameter values through atomics; structural changes (add/remove channel,
  insert, route) happen only while rendering is stopped or at a block boundary
  via a preallocated command queue.
- **UI thread:** reads meter snapshots from atomics.

## 4. Domain Model

All identifiers are `std::uint32_t`; `0` is invalid.

```cpp
enum class SourceKind { Hardware, SystemLoopback, VirtualInput };
enum class OutputKind { Physical, VirtualOutput };
enum class ChannelLayout { Mono, Stereo };

struct InsertSlot {
    std::uint32_t id;
    std::string pluginPath;
    std::string pluginName;
    bool bypassed;
};

struct Channel {
    std::uint32_t id;
    std::string name;
    SourceKind sourceKind;
    std::string sourceDeviceId;
    ChannelLayout layout;
    float gainDb;          // -60..+12, default 0
    float trimDb;          // -12..+12, default 0
    float pan;             // -1..+1, default 0
    bool muted;
    bool soloed;
    std::vector<InsertSlot> inserts;
};

struct Output {
    std::uint32_t id;
    std::string name;
    OutputKind kind;
    std::string deviceId;
    float gainDb;          // -60..+12, default 0
    bool muted;
    std::vector<InsertSlot> inserts;
};

struct Route {
    std::uint32_t channelId;
    std::uint32_t outputId;
    float sendGainDb;      // -60..+12, default 0
};

struct Scene {
    int schemaVersion;     // 1
    std::string name;
    double sampleRate;     // 44100, 48000, 88200, 96000; default 48000
    int bufferFrames;      // default 256
    std::vector<Channel> channels;
    std::vector<Output> outputs;
    std::vector<Route> routes;
};
```

### 4.1 Fixed rules

- **Gain:** dB, clamped to `[-60, +12]`. `-60` is treated as silence (`0.0`
  linear) at the fader.
- **Pan:** constant-power law. `theta = (pan + 1) * π / 4`;
  `leftGain = cos(theta)`, `rightGain = sin(theta)`. Center pan yields
  `0.70710678` on both sides (−3.01 dB).
- **Solo:** if any channel is soloed, non-soloed channels are silent. Mute wins
  over solo.
- **Send level:** applied post-fader, per route cell, linear
  `pow(10, sendGainDb / 20)`.
- **Summation:** multiple routes into one output sum; no automatic gain
  compensation.
- **Schema version:** a scene with an unsupported `schemaVersion` is rejected,
  never silently reinterpreted.
- **Unknown JSON fields** are ignored on load and dropped on save.

### 4.2 Failure semantics

| Failure | Behavior |
| --- | --- |
| Plugin fails to load or instantiate | Slot is marked bypassed, chain continues, warning surfaced |
| Plugin reports a processing error | Slot bypassed for the remainder of the session |
| Source device unavailable at scene load | Scene loads; that channel is stopped; warning surfaced |
| Output device unavailable at scene load | Scene loads; that output is disabled; warning surfaced |
| Hard plugin crash (in-process) | Not recoverable; engine restart required. Documented MVP limitation |

## 5. Scene Persistence

- JSON, one file per scene, `schemaVersion: 1`.
- Each insert slot persists the plugin's own state as an opaque
  base64 blob (`pluginState`). The core engine never interprets it: the host
  layer captures it when the scene is saved and replays it via the plugin's
  `setStateInformation` (before `prepareToPlay`) when the slot is restored.
  Slots saved without state keep the plugin defaults, so older files load
  unchanged.
- Location (host layer): `%APPDATA%\PluginRack\scenes\<name>.json`.
- Serialization is deterministic: stable key order, stable collection order.
- Loading never mutates the file and never throws on malformed input; it returns
  a result carrying errors.

## 6. Testing Strategy

- Every rule in §4.1 has a unit test with the exact numeric values above.
- The routing matrix is verified numerically (amplitude and channel placement),
  not by ear.
- Real-time safety is verified by counting allocations during `render`.
- Scene files are verified by round-trip equality and by rejecting malformed
  input.
- Device, driver, and plugin-host integration are verified by the later plans;
  the core engine takes test doubles for all three.

## 7. Delivery Plan

| Plan | Deliverable | Depends on |
| --- | --- | --- |
| 1 | Core engine library, scene format, offline render CLI — headless and fully tested | — |
| 2 | JUCE application shell, WASAPI capture/playback, system loopback, VST3/VST2 hosting | Plan 1 |
| 3 | Virtual audio endpoints and signed installer | Plan 2 |
| 4 | Mixer UI, insert editor, routing matrix, scene browser | Plan 2 |

Each plan produces software that runs and can be verified on its own. Plan 1 is
specified in `docs/superpowers/plans/2026-10-28-pluginrack-core-engine.md`.

## 8. Open Risks

1. **VST2 hosting** — SDK availability and licensing are unresolved. It is
   best-effort, never a release blocker.
2. **Virtual audio driver** — a signed kernel-mode or APO-based endpoint is
   required. This is the largest schedule risk and is isolated in Plan 3.
3. **In-process plugin crashes** — accepted for MVP; out-of-process hosting is a
   later architectural change, and the `IPluginInstance` seam must not assume
   in-process.
4. **Live-monitoring latency under load** — must be measured on real hardware
   during Plan 2, not assumed from the core engine's block size.
