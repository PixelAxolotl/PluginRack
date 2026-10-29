# Output FX Design (PluginRack)

Grill-session output, confirmed by user. First of three builds (then drag-reorder, then 6-band EQ).

## Goal

Every output/master strip gets the same 4-slot VST3 rack channels have: load, bypass, remove, editor window, persisted in scene.

## Decisions (locked)

- 4-slot mirror of the channel rack; single-slot rejected.
- Compact 120 px output strips (76 px cannot fit slots); master keeps current scroll-from-4th behavior.
- Tap point: output inserts run post-route-accumulation, pre-output-gain (mirrors channels' pre-fader inserts). Per-slot pre/post is out.
- InsertSlotComponent stops holding `ChannelStrip&`; both strips implement a small `IInsertHost` interface.
- Schema v1 → v2 with auto-migrate: v1 files load with empty output racks (+ flat EQ placeholder ignored until EQ plan), save writes v2.
- Engine caps strips at 4 inserts with a message (fixes live bug: 5th plugin is currently invisible but real).
- EQ on outputs, drag-reorder, presets, copy/paste: explicitly later, not here.

## Signal flow (app `AudioEngine::render`)

routes accumulate → **output inserts** → output gain/mute → meter → device push.
Core `MixerEngine` mirrors the same stage for CLI/tests.

## Latency

Report adds the clock output's insert latency sum (mirrors the first-input-channel rule).

## Acceptance

- 4 VSTs load/bypass/remove/edit per output; editors behave like channel editors.
- v1 scene loads, gains empty racks, saves as v2; v2 round-trips.
- 5th insert rejected with message on channels and outputs.
- Bypassed output insert passes audio unchanged.
- Removing an output with inserts drops chains + routes, render stays valid.
- Build `/W4 /WX` clean, all core tests green, screenshots at 1600×900 and 1180×700, smoke run alive, root `PluginRack.exe` refreshed.
