// PluginRackDriver.h — shared driver-private declarations.
// Kernel-only: compiled with the WDK, never with the user-mode build.
#pragma once

#include "PluginRackDriverProtocol.h"

#include <portcls.h>
#include <stdunk.h>

// {7A3B9C1D-4E2F-4A5B-8C6D-9E0F1A2B3C4D} — replace with a real GUID at fork.
DEFINE_GUID(GUID_PLUGINRACK_ADAPTER,
    0x7a3b9c1d, 0x4e2f, 0x4a5b, 0x8c, 0x6d, 0x9e, 0x0f, 0x1a, 0x2b, 0x3c, 0x4d);

// Private bridge interface: \\.\PluginRackBridge. ACL locked to local
// interactive/authenticated users (see the INF SDDL); the engine opens this
// to claim ring slots via IOCTL.
#define PLUGINRACK_BRIDGE_SYMLINK L"\\DosDevices\\PluginRackBridge"

// IOCTL: claim (slot, direction) for this process. Fails with
// STATUS_ALREADY_EXISTS while clientConnected is set.
#define IOCTL_PLUGINRACK_CLAIM_SLOT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
// IOCTL: release a claimed slot.
#define IOCTL_PLUGINRACK_RELEASE_SLOT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
// IOCTL: query header snapshot (magic, version, state, counters).
#define IOCTL_PLUGINRACK_QUERY_SLOT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA)

struct ClaimSlotInput {
    std::uint32_t slot;
    std::uint32_t direction; // pluginrack::driver::Direction
};

struct QuerySlotInput {
    std::uint32_t slot;
    std::uint32_t direction;
};

// One endpoint slot: WaveRT cyclic buffer (PortCls-owned) plus the shared
// SPSC ring (NonPagedPoolNx MDL, mapped into the engine process).
struct EndpointSlot {
    bool inUse = false;
    pluginrack::driver::Direction direction =
        pluginrack::driver::Direction::RenderToEngine;
    std::uint32_t slotIndex = 0;
    // Shared ring: header + interleaved float32 stereo payload.
    void* ringBase = nullptr; // system-mapped MDL address (DISPATCH_LEVEL safe)
    PMDL ringMdl = nullptr;
    std::uint32_t ringFrames = 0;
    // WaveRT side: cyclic buffer owned by the miniport stream.
    void* waveBuffer = nullptr;
    std::uint32_t waveFrames = 0;
    std::uint32_t wavePosition = 0; // position cursor, frames
    PEX_TIMER timer = nullptr; // ExSetTimer: drives position + bridging
    PKEVENT notifyEvent = nullptr; // connect/telemetry only, never per block
    volatile LONG clientConnected = 0;
};

// Adapter device extension: 2 render + 2 capture slots in v1.
struct AdapterExtension {
    PDEVICE_OBJECT physicalDeviceObject = nullptr;
    EndpointSlot render[pluginrack::driver::kRenderEndpointsV1];
    EndpointSlot capture[pluginrack::driver::kCaptureEndpointsV1];
};

extern "C" {
DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE AddDevice;
}

// Bridge dispatch: Create/Close/ DeviceIoControl on \\.\PluginRackBridge.
NTSTATUS BridgeCreate(PDEVICE_OBJECT deviceObject, PIRP irp);
NTSTATUS BridgeClose(PDEVICE_OBJECT deviceObject, PIRP irp);
NTSTATUS BridgeIoControl(PDEVICE_OBJECT deviceObject, PIRP irp);

// Timer callback: copies between the WaveRT cyclic buffer and the shared
// ring, advances the position cursor, signals notifyEvent on state change
// only. Runs at DISPATCH_LEVEL: no paging, no user pointers, all offsets
// clamped via clampAvailable before any copy.
void BridgeTimerCallback(PVOID context);
