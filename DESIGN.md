# PluginRack — Design System

**World:** Professional broadcast/studio console, VoiceMeeter Potato lineage. Not a consumer app, not a web dashboard. The surface is a control panel: dense, matte, legible under stage lighting, built to be read at a glance while audio is live.

**Mode:** Operate.

## Palette

| Token | Hex | Use |
| --- | --- | --- |
| `console-900` | `#1b2229` | window background, deep wells |
| `console-800` | `#232d36` | panel background |
| `console-700` | `#2b3742` | strip background, raised panels |
| `console-600` | `#35434f` | controls, button faces |
| `console-500` | `#465663` | control borders, inactive outlines |
| `line` | `#5d7183` | hairline dividers, inactive text |
| `text` | `#c9d6e0` | primary labels |
| `text-dim` | `#8fa3b3` | secondary labels, units |
| `fader-green` | `#7fd4a8` | faders, level meters, active routing |
| `fader-green-deep` | `#4a9b74` | fader track, meter bed |
| `mute-red` | `#e2574c` | mute, clipping |
| `solo-amber` | `#e8b84b` | solo, selected output |
| `accent-cyan` | `#4fb3c9` | focus, selected tab, plugin slot active |
| `vst-violet` | `#8f7fd4` | VST rack identity (replaces IntelliPan/FX) |

Text on `console-700`: `text` (#c9d6e0) on #2b3742 = 8.4:1. `text-dim` on #2b3742 = 4.6:1. Meets floor.

## Type

- One family: the system UI sans (JUCE default), semibold for labels, regular for values.
- Scale: 10 px micro labels (tracked +0.08em, uppercase), 11 px control labels, 13 px values, 15 px section titles, 22 px wordmark.
- Tabular numerals for gain, pan, dB, sample rate.

## Layout

- Header bar 56 px: wordmark left, hardware-out selector and clock right.
- Channel strip: 148 px wide, full height. Order top→bottom: source name + device, VST rack, level/pan, routing buttons, fader + meter, mute/solo.
- Master section fixed right: output strips A1–A5, B1–B3.
- Channel area scrolls horizontally; master stays pinned.
- 1 px hairlines between strips; no card shadows.

## Components

- **Fader:** vertical slot, green cap, dB readout above, 0 dB detent marked.
- **Meter:** vertical, green bed with a brighter peak segment; red above −3 dB.
- **Routing buttons:** A1–A5 / B1–B3 small squares, green when on.
- **VST rack:** 4 slots per channel. Each slot: index, plugin name or "empty", bypass toggle, remove. Active slot outlined in `vst-violet`. "＋" opens the VST3 picker.
- **Mute:** red when engaged, `mute-red`. **Solo:** amber when engaged.
- **Knobs:** pan and level, 34 px, drawn arc with indicator.

## Motion

150 ms ease-out on hover/press and meter decay only. No page-load choreography.

## States

Every control: default, hover (lighter face), pressed (inset), active (colored), disabled (40% opacity), focus (1 px `accent-cyan` ring). Empty VST slot reads "empty — click to load".
