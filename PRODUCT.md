# Product

<!-- impeccable:product-schema 1 -->

## Platform

desktop

Windows 10/11 x64 today, with macOS/Linux kept possible by the architecture. This
is a native JUCE application, not a web surface, so no web or mobile design
language applies.

## Users

Content creators and streamers replacing Voicemeeter. They run a live session —
usually stream or recording in progress — and need hardware inputs, system
playback and other applications routed through their own processing without the
routing model getting in the way.

## Product Purpose

PluginRack is a Voicemeeter-class audio mixer where every input can carry the
user's own VST plugins. It exists because Voicemeeter-style routing otherwise
locks users to a fixed set of built-in effects and a fixed channel layout.

Success is a live VST3 chain hosted in the routing path with low latency: the
user's own plugins, in their own order, on any input, without the mixer
introducing glitches or forcing a restart of the session.

## Positioning

The routing model a Voicemeeter user already knows (many-to-many sends with an
independent level per channel→output cell), combined with genuine VST3 hosting
and an open-ended channel count. A fixed-effect router cannot truthfully copy it,
and a DAW does not offer this routing surface.

## Operating Context

- Windows desktop application, keyboard and mouse, used during live sessions
  where an interruption is audible.
- The engine runs one device clock for the whole app: one sample rate and buffer
  size, negotiated against the devices.
- Sources include hardware capture devices, system loopback and virtual input
  endpoints; destinations are physical devices and virtual output endpoints.
- Sessions are worked on as scenes: channel names, gains, pan, mute/solo, EQ,
  compressor/gate, insert plugin state, routing sends and device choices, saved
  and reloaded as JSON.
- Latency is measured from inside the app (round-trip measurement) and reported
  to the user.
- Repository carries its own spec and handoff notes under `docs/`; the build and
  test commands live in `docs/NEXT_SESSION_MIXER.md`.

## Capabilities and Constraints

Confirmed by the implementation and its design spec:

- Per-strip insert chain of up to six slots, hosted in-process, ordered with built-in effects.
- Channel processing: six-band EQ with delay, a compressor and a gate. The
  compressor and the gate share one `Dynamics` editor and are drawn as one
  `Dynamics` card with a bypass switch each; the EQ keeps its own card. There
  is no limiter.
- Mixer controls: input trim, gain, pan, mute, solo, peak metering.
- Many-to-many routing matrix; each channel→output cell is a send with its own
  level. Outputs can be added and removed at runtime.
- Mono and stereo channels; mono is upmixed to stereo for processing.
- VST3 required; VST2 best-effort behind the same host interface.
- Real-time constraints are binding: the audio thread must not allocate, and UI
  work must never cost audio. Meter, analyzer and graph meters read from
  lock-free taps.

Terminology in use: `strip`, `insert`, `Channel FX` (EQ plus Compressor/Gate),
`output`, `scene`, `saves`.

Undecided, and not to be invented:

- Distribution model (personal tool versus public release), installer, licensing
  and pricing.
- If or when macOS and Linux builds ship.

## Evidence on Hand

- `docs/superpowers/specs/2026-10-28-pluginrack-design.md` — approved design
  specification: problem, MVP scope, routing model.
- `docs/NEXT_SESSION_MIXER.md` and further notes under `docs/superpowers/` —
  build commands, SDK paths, handoff state.
- No marketing copy, screenshots, testimonials, benchmarks or brand assets exist
  in the repository. Do not fabricate any.

## Product Principles

1. **Routing is the product.** The Voicemeeter routing model — many-to-many,
   per-cell send level, open-ended inputs and outputs — is the reason the app
   exists; every screen serves it.
2. **The user's plugins are the effects.** Built-in processing is housekeeping
   around a VST3 chain, never a substitute for what the user loaded.
3. **Live means live.** A session is running while it is being edited: no
   blocking work in the audio path, no UI action that interrupts sound.
4. **One session, fully recalled.** Scenes must bring back exactly what was set,
   including plugin state and routing, because losing a live setup is the worst
   failure the product can have.
5. **Power without a manual.** A Voicemeeter user should recognise the mixer
   immediately; depth is added beneath familiar controls, not by inventing a new
   vocabulary.
