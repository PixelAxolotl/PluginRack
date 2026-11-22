# Noise Gate design

## Goal

Add a built-in effect named **Noise Gate** that reduces room tone, hiss, and other low-level noise between wanted sounds. It should be available on channel and output strips, have its own insert-chain position and controls, and save with the scene. It is a downward expander, not a spectral noise remover: noise under speech or music will not be removed independently of the wanted sound.

## Existing code and reuse

`DynamicsEffect` already represents compressors and gates, and `DynamicsProcessor` already applies a downward-expansion curve for `DynamicsEffectType::Gate`. The separate effect should reuse that DSP behavior rather than introduce a second gate algorithm. It will have its own scene settings and chain identity so it can be added, moved, bypassed, edited, and removed independently of the existing Dynamics effect.

Follow the existing built-in effect patterns for scene serialization/validation, channel and output rendering, chain menus/cards, and the focused effect window. Do not change the existing Dynamics compressor or gate behavior.

## User-facing behavior

- Add **Noise Gate** as a built-in item to channel and output insert chains.
- Show a standalone gate card/chain row with its own bypass and remove actions; opening it shows the Noise Gate controls.
- Controls: **Threshold**, **Range**, **Attack**, **Release**, and **Knee**. Threshold decides when attenuation begins; Range limits maximum noise reduction; Attack governs opening behavior; Release governs return to attenuation; Knee softens the threshold transition.
- Initialize a newly added Noise Gate enabled (not bypassed), with conservative values: threshold −45 dB, range 24 dB, attack 5 ms, release 150 ms, and soft knee 6 dB. These are starting values, not a promise that one setting removes every noise floor; users adjust threshold relative to their source.
- The effect processes the strip's normal stereo signal, is applied at its selected chain position, and does not add latency or allocate in the audio callback.
- Changes and bypass state update the running engine and persist through normal scene save/load. Scenes without this effect continue to load unchanged.

## Data and audio path

- Add a dedicated `NoiseGateEffect` value type and a per-strip list. Include an id, bypass state, threshold, range, attack, release, and knee. Bound parameter values in scene validation to the editor's documented ranges; reject non-finite values.
- Add a `NoiseGate` chain-item type. Resolve the chain item's id against its strip's noise-gate list and process in order. Reuse `DynamicsProcessor` by translating the gate settings to a `DynamicsEffect` with `type == Gate`; do not implement or fork the gain-computer.
- Apply the same effect state and processing to both input channels and outputs. Keep processor state per strip/effect; prepare/reset it alongside other strip DSP when sample rate or effect topology changes.
- Extend JSON encoding/decoding additively. Missing noise-gate fields mean an empty list, preserving older scenes. Include round-trip, invalid-parameter, dangling/duplicate-chain-reference coverage.

## UI and updates

- Add the item to the built-in plugin/effect menu and chain presentation for channels and outputs.
- Reuse the established built-in effect editor patterns for a dedicated window and live parameter updates. Bypass, edits, reorder, and removal must remain synchronized with the scene model and audio engine, including when an editor is reopened.
- Use the existing project styling and value-edit controls; do not add a dependency or a separate analyzer/visualizer for this first version.

## Validation

- Unit-test the reused gate DSP mapping and verify attenuation below threshold is bounded by Range, signals above threshold approach unity gain, knee is smooth, and bypass is transparent.
- Test scene round-trip and validation, chain-order processing, and both channel/output integration.
- Run the focused tests and full test suite, then run the required repository build-and-copy script so the root `PluginRack.exe` matches the sources.

## Scope exclusions

- No spectral subtraction, learned noise profile, voice activity detection, expander sidechain, lookahead, adaptive threshold, or automatic calibration.
- No changes to the existing Dynamics gate defaults or UI.
- No claim of removing noise while wanted audio is present; this design only attenuates quieter intervals.
