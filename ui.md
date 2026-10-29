# PluginRack UI — Physical Input Assignment

## Channel input mapping

Each channel strip exposes an input device selector (DeviceControl) plus a
layout/mapping row:

- **Mode combo**: `Mono` or `Stereo`.
- **Input A / Input B combos**: 1-based physical input indices into the
  selected device's capture. In Mono mode only Input A is used; in Stereo
  mode Input A and Input B feed L/R. If both are set to the same index the
  second is auto-adjusted.

## Shared capture

Multiple logical channels can share one device capture. `AudioEngine` opens
the device once at the highest physical index any mapped channel needs and
broadcasts frames to all channels mapping into it. Changing a channel's
mapping calls `setChannelMapping` / `setChannelInputDevice`; the engine
re-opens or reuses the capture as needed.

## Split into mono channels

The `Split` button on a stereo (or multi-input) channel creates one mono
channel per physical input of the assigned device, naming them `Input 1`,
`Input 2`, ... and mapping each to a single physical input. The original
strip is converted to mono on input 0.

## Routing

Per-channel routing toggle grid in the mixer strip sends to outputs. Full
matrix editing is available in the Routing view. Send gain defaults to
0 dB when toggled on and -60 dB when off.