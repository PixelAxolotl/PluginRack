# Audio buses (DAW-style group / parallel buses)

Date: 2026-11-23
Status: awaiting approval
Schema: 9

## Intent and why

PluginRack today routes channels straight to outputs. The only "bus" in the
scene is `AuxBus`, a side-chain key mixer: it sums channels, applies one gain,
and any insert can use it as a side-chain trigger. It has no fader, no inserts,
no EQ, no meter, and its audio never reaches an output.

The user wants proper DAW buses for **bus compression and bus effects**, with
this routing behaviour (their sketch, confirmed):

```
Input 1 --send--> Bus 1 [comp / inserts, fader, meter] --> Output 1
   `--------------> Output 1   (dry path stays open)
```

Feeding a bus does **not** close the channel's direct output path. A channel can
run dry into the output and in parallel through a processed bus; muting the
direct send turns it into a classic group bus.

## Goals

- A bus is a full strip: inserts, built-in effect chain, EQ, fader, mute,
  meter, name, colour, and its own insert hosting.
- Channels send to buses with an independent per-cell send level (split L/R
  like `Route`), and buses route to one or more outputs the same way.
- Channel -> bus and channel -> output are independent cells: parallel dry +
  wet and group-only routing are both expressible.
- Bus audio is audible: it is summed into outputs, not just a key signal.
- A bus with no output route stays key-only and remains selectable as a
  side-chain source, so every existing session keeps working.
- The existing side-chain `Bus` entries become these buses; the UI label stays
  "Bus".
- Live engine and offline `MixerEngine` render the same routing.
- Scene schema 9 with lossless migration from schema 8.

## Non-goals

- No bus -> bus nesting in v1. Sends are channel -> bus and bus -> output only.
- No bus solo in v1. Buses have mute; channel solo keeps its current meaning.
- No pre-fader sends in v1. Sends tap the channel's post-fader signal, matching
  the current routing matrix and side-chain `PostFader` default.
- No bus strips on the I/O page. I/O stays "inputs" and "outputs".
- No Racks-page bus selection in v1. `RackView` selects with
  `setSelectedChannel(int id, bool isOutput)`, which cannot address a bus
  without an id-space collision; bus inserts and built-in editors are opened
  from the bus strip itself, as output inserts already are.
- No new plugin delay compensation. Bus inserts add latency exactly the way
  output inserts do today.
- No bus reordering in v1 (add / remove only).

## Constraints

- Real-time safety: no allocation on the audio thread. Bus buffers, taps,
  side-chain buffers and insert chains are sized in the prepare path. Live
  values are atomics; structural state is written on the message thread under
  `stateLock`.
- Both engines must agree on the render order.
- The scene is the only persistence format for the app; a schema bump must
  accept every older version and reject anything newer.
- AGENTS.md: build and copy `PluginRack.exe` after every source or config
  change; Conventional Commits; no AI-attribution trailers.

## Approaches considered

1. **Bus as a third strip kind (`Bus : Strip`)** *(chosen)*. `Strip` already
   holds inserts, every effect family, chain order, EQ, gain, mute, colour and
   analyser offsets. Channels and outputs already render through the same
   per-item chain code. A `Bus : Strip` inherits all of it; only routing and
   presentation are new. Cost: one schema bump and new caches in both engines,
   but no duplicated DSP.
2. **Bolt fields onto `AuxBus`**. Smaller first diff, but insert hosting, EQ,
   chain processing, metering and the strip UI would be re-implemented or
   duplicated, and the result converges on option 1 with more code.
3. **Separate "audio bus" type, leave `AuxBus` alone**. Lowest migration risk,
   but two things called "Bus" in the UI that cannot mix, doubled summing code,
   and side-chain inserts could not key off the new buses.

Option 1 was picked on 2026-11-23.

## Data model (schema 9)

`core/include/pluginrack/Scene.h`:

```cpp
struct Bus : Strip {};                 // no extra fields; order = mixer order

struct BusSend {                       // channel -> bus
    std::uint32_t channelId = 0;
    std::uint32_t busId = 0;
    float sendGainDb = 0.0f;
    std::optional<float> sendGainDbR;  // absent = same as left
    bool operator==(const BusSend&) const = default;
};

struct BusRoute {                      // bus -> output
    std::uint32_t busId = 0;
    std::uint32_t outputId = 0;
    float sendGainDb = 0.0f;
    std::optional<float> sendGainDbR;
    bool operator==(const BusRoute&) const = default;
};

struct Scene {
    ...
    std::vector<Bus> buses;
    std::vector<BusSend> busSends;
    std::vector<BusRoute> busRoutes;
    // auxBuses removed from Scene; its JSON key is read for migration only.
};
```

- Bus ids are non-zero and unique among buses. They do not collide with channel
  or output ids because `SideChainSource` already namespaces the reference.
- Send/route gains reuse the existing `kMinGainDb`..`kMaxGainDb` range and the
  optional right-side field convention from `Route`.
- `kSchemaVersion` becomes 9.

### Creation, storage and caps

Sends and routes are **sparse and created on first edit**, unlike the dense
channel-to-output `routes` the engine seeds today:

- A missing send/route displays as `kSendOffDb` (-60 dB), the same value an
  absent channel route already reads as.
- `setBusSend` / `setBusRoute` create the entry when it is missing. A new
  `addBus` starts with no sends and no routes, so it is key-only until the user
  wires it. `SideChainBar`'s `+ Bus` seeds the chosen source channels at 0 dB
  right after creating it.
- `Sources` on creates a 0 dB send; off erases it.
- Adding a channel adds no sends; adding an output adds no routes. Removing a
  channel/output/bus erases every send or route that names it.
- Why sparse: a new bus must not become audible the moment it exists, and a
  key-only bus must be expressible without dense -60 dB entries. It also keeps
  scene files small and keeps the cap from being consumed by unused cells.

Caps, enforced by both the engine and the validator:

- `kMaxBuses = 16`
- `kMaxBusSends = 1024`
- `kMaxBusRoutes = 512` (the full 16 x 32 matrix)

A create call that would exceed a cap is refused (returns false; the UI reports
it) so no in-memory scene can fail validation on save.

### Migration

When reading schema 8 (or any older scene that carries the v8 keys):

- Each `AuxBus {id, name, gainDb, sourceChannelIds}` becomes
  `Bus {id, name, gainDb}` plus one `BusSend {channelId, busId: id,
  sendGainDb: 0.0f}` per source channel.
- `busRoutes` stay empty, so the bus is key-only and never reaches an output.
- Insert side-chain references (`SideChainSource::Bus`, `sideChainSourceId`)
  keep working unchanged: the id is preserved.
- Every insert whose `sideChainSource == Bus` is rewritten to
  `sideChainTap = PostFader`. The old bus key ignored the tap and always used
  the post-gain bus signal, so a stored `PreInsert` tap would otherwise lose
  `gainDb` after migration.
- With no inserts/EQ on a migrated bus, `postFaderTap` equals the old
  `sum x gainDb` key signal, so the key path is behaviour-preserving.

Writer: emit `schemaVersion: 9`, `buses`, `busSends`, `busRoutes` (each only
when non-empty, matching the existing optional-key style) and no `auxBuses`.

Reader: accept `schemaVersion` 1..9. Schema <= 7 still clears side-chain state
and never invents buses.

### Validator

New errors in the existing `SceneErrorCode` style:

- bus id zero / duplicate bus id
- send references unknown channel or unknown bus
- duplicate `(channelId, busId)` send
- route references unknown bus or unknown output
- duplicate `(busId, outputId)` route
- send/route gain out of range, non-finite right gain
- bus strip validated through the existing `validateStrip` path
- side-chain `Bus` references an existing bus id

Also remove the `auxBuses` checks and validate the migrated shape instead.

## Rendering

### Order in one block

Steps 1-4 and 6-7 run in both engines; step 5 is live-only because the core
`MixerEngine` has no side-chain runtime today (channel and output keys are
already ignored offline).

1. Pull channel inputs; clear output buffers.
2. Process every channel exactly as today, including the channel chain and the
   post-fader/pan write into `postFaderTap`.
3. **Buses:** clear each bus buffer, then for every `BusSend` add the source
   channel's current-block `postFaderTap` at the send gain (L and R
   independently; a mono channel's left leg feeds both sides, matching the old
   aux-bus summing).
4. **Bus strips:** copy the sum into `preInsertTap`, run the bus chain in scene
   order (inserts, EQ, dynamics, multiband, de-esser, limiter, feedback
   suppressor, spectral denoiser), apply fader and mute, write
   `postFaderTap`, update the bus meter and RTA taps.
5. **Side-chain fill (live engine only):** a `Bus` source reads the bus
   `preInsertTap` for `PreInsert` and `postFaderTap` for `PostFader`; channel
   and output sources keep today's behaviour. Bus buffers are computed after
   channels, so an insert inside a channel that keys off a bus reads the
   previous block's bus tap — the same one-block relationship the old `AuxBus`
   had. Bus inserts keying off their own bus read the current block. Output
   inserts keying off a bus now read the current block's tap instead of the
   previous block's; the shift is one block (a few milliseconds) and accepted
   as inaudible.
6. **Routes:** channel -> output (existing) and bus -> output both add the
   post-fader tap into the destination buffer with L/R send gains.
7. Process every output exactly as today.

A bus with no `BusRoute` contributes nothing to any output but is still
rendered and metered, which is what keeps key-only buses useful.

### Live engine (`app/src/AudioEngine.h` / `.cpp`)

- Add `struct Bus` mirroring the channel/output state that the strip needs:
  id, name, `Atomic<float> gainDb`, `Atomic<bool> muted`, peak/RMS atomics,
  inserts, chain, every effect + processor vector, EQ, taps, analyser taps,
  colour/RTA offsets, `AudioBuffer` + `preInsertTap` + `postFaderTap`.
- Add `struct BusSend { channelId; busId; Atomic<float> gainDbL, gainDbR; }`
  and `struct BusRoute { busId; outputId; Atomic<float> gainDbL, gainDbR; }`.
- Replace `auxBuses` / `AuxBus` / `routes`-only state with `buses`,
  `busSends`, `busRoutes`, plus `nextBusId`.
- `prepareToPlayImpl`: size bus buffers/taps, EQ, processors, side-chain
  buffers and insert hosts exactly like outputs.
- Message-thread API (under `stateLock`):
  `addBus(name)`, `removeBus(id)`, `setBusName/Gain/Muted`, `setBusSend*`,
  `setBusRoute*`, `getBusSends/BusRoutes`, and the insert/chain/EQ/effect
  setters currently prefixed `setOutput*` get `Bus` twins (or a shared
  implementation taking the strip).
- `removeChannel` also erases `busSends` for that channel; `removeOutput`
  erases `busRoutes` for that output; `removeBus` erases its sends/routes and
  points any insert whose side-chain source is that bus back to `None`
  (otherwise the scene would fail validation on save).
- `buildScene` writes buses/sends/routes; the restore job rebuilds them and
  recomputes `nextBusId`.
- `MeterBank`: add a bus table (`observeBus`, `busPeak`, `observeBusRealtime`,
  and a `setTargets` overload). The fixed-slot design already supports a third
  table.
- `SideChainBar` forward declarations (`addAuxBus`/`removeAuxBus`/`setAuxBus`)
  are replaced by the bus API.

### Offline engine (`core/src/MixerEngine.cpp`)

- Add `BusCache` (gain/mute), `BusSendCache`, `BusRouteCache`, per-bus
  `StripDsp`, bus insert chains, and bus buffers.
- Prepare sizes all of it; render mirrors steps 1-4 and 6-7. Side-chain taps
  are live-only, so no key input or tap buffers are added to core.
- `MeterBank` bus table is registered alongside channels/outputs.
- `OfflineRenderer` needs no API change; tests compare bus routing through it.

## UI

### Mixer (`MixerComponent`)

- New lane between INPUTS and OUTPUTS, visible only in `Sections::Both`,
  with header "BUSES" and a `+ Bus` button in the toolbar.
- `BusStrip`: same visual grammar as `OutputStrip` minus device controls —
  name, remove button, VST rack, EQ/dynamics/multiband/de-esser/limiter/
  feedback/denoiser buttons, fader, mute, stereo meter. No pan (buses are
  stereo sums) and no solo.
- Insert slots, chain drag/reorder and plugin editor windows come from the
  same `IInsertHost` interface output strips already implement.
- Double-click / selection on a bus does not call `RackView`.

Implementation note: `OutputStrip` is ~950 lines. Prefer extracting the common
strip body into a shared base or a `StripHost` helper so `BusStrip` does not
copy it. If extraction turns risky, clone it first and trim device-only parts;
the plan can decide with the reviewer.

### Routing (`RoutingMatrixComponent`)

- One grid: columns are buses then outputs; rows are channels then buses.
  Header bands label `BUSES` and `OUTPUTS`.
- Channel rows: bus cells + output cells. Bus rows: output cells only; cells
  where a route cannot exist are painted empty, not interactive.
- Each cell keeps the existing `ValueEditSlider` behaviour (L/R editing as the
  current matrix exposes it).
- Worst case is 64x16 + 64x32 + 16x32 = 3584 sliders. That is heavier than
  today's 2048, so the plan should measure rebuild time; if it is slow, the
  fallback is two sub-grids ("Channels -> Buses" and "Buses -> Outputs") or
  building bus columns lazily.

### SideChainBar

- The bus list reads `AudioEngine::getBuses()`.
- `SideChainBar`'s owner becomes `enum class InsertOwnerKind { Channel, Output,
  Bus }` instead of the current `bool isOutput`; `setTarget` and
  `getSideChainKeyPeak` take that kind so bus-owned inserts can key off a bus.
- `+ Bus` creates an empty bus, then seeds sends by owner: the owner channel for
  a channel insert, every current channel for an output insert (matching
  today's default), and no sends for a bus insert (a bus keying its own insert
  adds nothing).
- `Sources` toggles channel membership (0 dB on, erase the send off) and keeps
  "Remove this bus". Buses never feed buses, so the list shows channels only.
- The "Key" meter is unchanged; a bus strip also shows its own output meter.

## Tests

- `tests/test_scene_codec.cpp`
  - v8 scene with `auxBuses` loads as buses + 0 dB sends for the old sources
    (no other channels), no routes, preserved ids/names/gains.
  - migration rewrites inserts keyed by a bus to `sideChainTap = PostFader`.
  - v9 round-trip (buses, sends with split L/R, routes).
  - v10 rejected; v7 still clears side-chain state and creates no buses.
- `tests/test_scene_validator.cpp`
  - every new error above (bad ids, duplicates, unknown references, gains).
  - migrated v8 scene passes validation.
- `tests/test_mixer_engine.cpp`
  - `addBus` creates no sends or routes (key-only by default).
  - `setBusSend` / `setBusRoute` create a missing entry; `Sources` off erases.
  - creation past `kMaxBuses` / `kMaxBusSends` / `kMaxBusRoutes` is refused.
  - channel -> bus -> output renders the processed sum;
  - parallel dry + bus sums both paths;
  - bus mute silences only the bus path;
  - independent L/R send gains;
  - bus with no route contributes nothing to outputs;
  - bus meter reflects bus level.
- `tests/test_offline_render.cpp`
  - a scene with a bus renders through `OfflineRenderer`.

Manual check after build: add two channels, route both to a bus, put a
compressor on the bus, keep one channel's direct output open, confirm the other
is group-only; verify meters, scene save/reload, and an old v8 scene still
opens with its side-chain bus working.

## Acceptance

- A bus can be created, named, fed by channels, given inserts/EQ/effects, and
  routed to an output.
- Parallel dry + bus and group-only routing both work without a mode switch.
- Existing side-chain buses survive load/save with the same key level; the only
  timing change is an output insert keying off a bus reading the current block
  instead of the previous one.
- Offline parity covers bus sum, strip chain, fader/mute, routes and meter;
  side-chain keying stays live-only, as it already is for channels and outputs.
- Offline tests and the app build are green (`tools/build-and-copy.ps1`).
