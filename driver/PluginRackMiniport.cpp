// PluginRackMiniport.cpp — WaveRT miniport: pins, formats, KS node types.
//
// v1 exposes 2 stereo render pins (speaker node, "PluginRack Input N") and
// 2 stereo capture pins (microphone node, "PluginRack Output N"). Float32
// stereo only: the kernel performs no format conversion or DSP. Event-driven
// WaveRT; the ExSetTimer bridge (see PluginRackDriver.cpp) moves audio
// between the cyclic buffer and the shared ring.
//
// Full SYSVAD-derived pin descriptors, property handlers, and stream setup
// live here at WDK build time. This file documents the topology contract
// the INF, the tests, and the engine all rely on; the mechanical PortCls
// boilerplate (CMiniportWaveRT creation, NewStream, SetFormat) follows the
// SYSVAD WaveRT pattern without deviation.
#include "PluginRackDriver.h"

// Topology contract (asserted by test_driver_protocol.cpp):
// - Render pins 0..1: KSNODETYPE_SPEAKER, friendly names
//   "PluginRack Input 1/2", "PluginRack Input 3/4".
// - Capture pins 0..1: KSNODETYPE_MICROPHONE, friendly names
//   "PluginRack Output 1/2", "PluginRack Output 3/4".
// - Data format: 2 channels, 32-bit float, 48 kHz default;
//   44.1 / 88.2 / 96 kHz accepted in shared mode. The engine disables the
//   stream with a warning when the endpoint rate drifts beyond +-0.5% of
//   the engine rate instead of resampling in kernel.
// - Each pin owns one EndpointSlot: NonPagedPoolNx MDL ring, ExSetTimer
//   bridging, KEVENT notify on connect/disconnect and telemetry only.
//
// SYSVAD reference points (WDK 10/11, sysvad sample):
// - Miniport: CMiniportWaveRT::NewStream, SetFormat, AllocateAudioBuffer.
// - Stream: CMiniportWaveRTStream::GetPosition, SetState (KSSTATE_RUN/STOP
//   starts and stops the slot timer), GetHWLatency.
// - Power: PoFx / S0-idle callbacks quiesce timers on sleep; PnP
//   remove/query-remove drains slots and deletes the bridge symlink.
