# Next Session: Mixer UI Rework (PluginRack)

Handoff notes prepared 2026-10-30. Read alongside `DESIGN.md`, `PRODUCT.md`, `ui.md`.

## Project

- Windows desktop audio mixer (JUCE), Voicemeeter-class router with user VST3 plugins.
- Stack: C++20, JUCE, CMake (Ninja), MSVC 14.51, VST3 SDK.
- Build: `cmake --build build/dev --target pluginrack_app` (must run under vcvars64). Tests: `--target pluginrack_tests`.
- SDK paths: `C:/Users/thorz/JUCE`, `C:/Users/thorz/vst3sdk`.

## What's already done (this session)

Rack view rebuilt to 4-column wireframe (see `app/src/RackView.cpp/.h`):
- Left channel strip: channel combo selector, name (right-click to rename), sub-label, trim/gain knob (-12..+12 → `setChannelTrim`), vertical level fader + `LevelMeter`, rotary pan knob, Mute/Solo/Mixer buttons.
- Plugin column: "Inserts" header, dedicated `EqButton` EQ slot (left-click bypass, right-click docks EQ editor in center), vertical VST slot list, +Add VST3.
- Center: docked plugin editor (VST or embedded `EQWindow` in `embedded` mode).
- Right: output routing toggle buttons (send gain 0/-60 dB).
- `EQWindow` gained an `embedded` constructor flag so it can be hosted as a child component.
- `AudioEngine` gained `setChannelName` (and reused `setOutputName`); duplicates cleaned up.
- Bug fixed: clicking a VST after docking EQ now restores the VST UI (force `refreshEditor()`).
- Tests pass (5447 assertions / 80 cases). Binary copied to repo root.

## Remaining task: Mixer UI update (image 2 wireframe)

Goal: rework `app/src/MixerComponent.cpp/.h` to match the hand-drawn mixer wireframe (image 2). The wireframe is schematic — only one channel drawn as an example; additional channels follow the same pattern.

### Wireframe layout (image 2)

Top bar:
- Left: wavy menu/hamburger.
- Center: 4 small parallelogram/trapezoid shapes (window controls or view tabs).

Second header row:
- Left: `+Ch` (add channel), right: `Out+` (add output), full-width separator below.

Body — three zones:
1. Left input channel strip (narrow), per channel labeled `A1`, `Input v` dropdown:
   - Gain knob: circle with `-12` / `+12`.
   - Pan knob: circle with `L` / `R`.
   - To the right of knobs: tall input-select list box (device/input mapping) with scribbles + diagonal line at bottom.
   - Bottom: long vertical channel fader with ticks (cap ~1/4 down), thinner vertical meter bar to its right, `M` and `S` buttons in rounded boxes at bottom.
   (No 48V phantom button — skip; not a physical mixer.)
2. Center: large empty rectangle — candidate host for the routing matrix (channels x outputs) OR leave as canvas. Decision needed.
3. Right output section:
   - Header: `A1 A2 A3 ...` output columns/tabs.
   - Each column: top dropdown (`v`), middle list/routing box with horizontal lines; A1 fully drawn has a vertical fader with ticks in the lower half + a `mute` button at the bottom.
   - Vertical dividers between columns.

### Current MixerComponent (for reference)

- Horizontal scrolling channel strips (148 px) in `channelViewport` → `stripContent`.
- Master/output strips (120 px) on the right in `masterViewport` → `masterContent`.
- Each `ChannelStrip`: device control, mode combo, input A/B combos, 4 VST slots, vertical gain (level) knob (trim), level fader + meter, mute/solo, EQ button, routing buttons.
- `OutputStrip`: device control, mode combo, flip, 4 VST slots, vertical gain fader + meter, mute, EQ.
- Routing is also a separate view (`RoutingMatrixComponent`, `AppView::Routing`).

### Open decisions (grill before coding)

1. Left side: all channels stacked vertically (recommended) vs. selected channel only.
2. Center content: embed `RoutingMatrixComponent` (recommended) vs. leave empty canvas (routing stays in its own tab).
3. Arrangement of per-channel controls: keep current `ChannelStrip` internals (gain knob, pan, fader, meter, M/S, EQ, routing) but lay strips vertically, or redesign each strip to match the wireframe (gain knob top, pan, fader+meter bottom, M/S).
4. Whether to keep the separate Routing tab once the matrix is embedded in the mixer.

### Suggested approach

- Prefer reusing `ChannelStrip` / `OutputStrip` components rather than rewriting; change only the outer layout in `MixerComponent::resized()` and strip stacking.
- If embedding the routing matrix, instantiate `RoutingMatrixComponent` in the center area and call its refresh on structure changes.
- Keep `onSelectionChanged` wiring to `RackView::setSelectedChannel` intact.
- Preserve `onStructureChanged` callback for scene save/restore.

### Key files

- `app/src/MixerComponent.h/.cpp` — main target.
- `app/src/ChannelStrip.h/.cpp` — per-channel strip (already has gain knob, pan, fader, meter, M/S, EQ, routing).
- `app/src/RoutingMatrixComponent.h/.cpp` — candidate for center embedding.
- `app/src/MainComponent.cpp` — view routing / wiring.
- `DESIGN.md` — palette/tokens (console-*, fader-green, mute-red, solo-amber, accent-cyan, vst-violet).
- `ui.md` — input mapping / routing notes.

### Build & verify

```
# from repo root, under vcvars64
cmake --build build/dev --target pluginrack_app
cmake --build build/dev --target pluginrack_tests
build\dev\tests\pluginrack_tests.exe
```

Copy binary: `Copy-Item build\dev\app\pluginrack_app_artefacts\Release\PluginRack.exe PluginRack.exe -Force`
Run checker before commit.

### Notes / gotchas

- JUCE `Atomic<float>` has no `.load()` in this version; use `audioEngine.getSendGain(ch, out)` instead of reading routes directly.
- `ChannelStrip` stores `channel` by reference; rebuilds destroy and recreate strips — preserve indices.
- Output EQ is keyed by scene position (index), not id; resolve by id at edit time (see `MixerComponent::OutputStrip::openEqWindow`).
- Scene v3: `eqBypassed` defaults true; `trimDb` is session-only (not persisted).
- Do not add 48V phantom (software router, not hardware).
- Avoid `Co-authored-by` / AI attribution trailers in commits.