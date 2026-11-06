// PluginRackDriver.cpp — DriverEntry, AddDevice, PnP, bridge interface.
//
// Build with the WDK (not the plain Windows SDK): needs portcls.lib,
// stdunk.lib, and km headers. See driver/README.md for the exact WDK
// version, build command, and test-signing steps.
#include "PluginRackDriver.h"

namespace {

// Locked-down SDDL for the bridge interface: local interactive and
// authenticated users only, so unprivileged processes cannot snoop audio
// or corrupt ring state.
constexpr const wchar_t* kBridgeSddl =
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)(A;;GRGW;;;AU)";

EndpointSlot* findSlot(AdapterExtension* ext, std::uint32_t slot,
                       pluginrack::driver::Direction dir) {
    if (dir == pluginrack::driver::Direction::RenderToEngine) {
        if (slot >= pluginrack::driver::kRenderEndpointsV1) return nullptr;
        return &ext->render[slot];
    }
    if (slot >= pluginrack::driver::kCaptureEndpointsV1) return nullptr;
    return &ext->capture[slot];
}

} // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject,
                                PUNICODE_STRING /*registryPath*/) {
    driverObject->DriverExtension->AddDevice = AddDevice;
    driverObject->DriverUnload = nullptr; // PortCls handles unload
    return PcInitializeAdapterDriver(driverObject, nullptr, nullptr);
}

extern "C" NTSTATUS AddDevice(PDEVICE_OBJECT physicalDeviceObject) {
    // PortCls creates the functional device object and attaches the
    // adapter; the miniport (see PluginRackMiniport.cpp) exposes the
    // WaveRT pins. Endpoint names, KS node types (speaker on render,
    // microphone on capture), and the bridge SDDL live in the INF.
    return PcAddAdapterDevice(physicalDeviceObject, nullptr, 0, nullptr,
                              GUID_PLUGINRACK_ADAPTER);
}

NTSTATUS BridgeCreate(PDEVICE_OBJECT /*deviceObject*/, PIRP irp) {
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS BridgeClose(PDEVICE_OBJECT /*deviceObject*/, PIRP irp) {
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS BridgeIoControl(PDEVICE_OBJECT deviceObject, PIRP irp) {
    auto* ext = static_cast<AdapterExtension*>(
        deviceObject->DeviceExtension);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_PLUGINRACK_CLAIM_SLOT: {
        if (stack->Parameters.DeviceIoControl.InputBufferLength <
            sizeof(ClaimSlotInput)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        auto* in = static_cast<ClaimSlotInput*>(
            irp->AssociatedIrp.SystemBuffer);
        auto* slot = findSlot(
            ext, in->slot,
            static_cast<pluginrack::driver::Direction>(in->direction));
        if (slot == nullptr) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        // Single client per slot: second opener rejected until release.
        if (InterlockedCompareExchange(&slot->clientConnected, 1, 0) != 0) {
            status = STATUS_ALREADY_EXISTS;
            break;
        }
        auto* h = static_cast<pluginrack::driver::SharedRingHeader*>(
            slot->ringBase);
        if (h != nullptr) h->clientConnected = 1;
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_PLUGINRACK_RELEASE_SLOT: {
        if (stack->Parameters.DeviceIoControl.InputBufferLength <
            sizeof(ClaimSlotInput)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        auto* in = static_cast<ClaimSlotInput*>(
            irp->AssociatedIrp.SystemBuffer);
        auto* slot = findSlot(
            ext, in->slot,
            static_cast<pluginrack::driver::Direction>(in->direction));
        if (slot == nullptr) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        InterlockedExchange(&slot->clientConnected, 0);
        auto* h = static_cast<pluginrack::driver::SharedRingHeader*>(
            slot->ringBase);
        if (h != nullptr) h->clientConnected = 0;
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_PLUGINRACK_QUERY_SLOT: {
        if (stack->Parameters.DeviceIoControl.InputBufferLength <
                sizeof(QuerySlotInput) ||
            stack->Parameters.DeviceIoControl.OutputBufferLength <
                sizeof(pluginrack::driver::SharedRingHeader)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        auto* in = static_cast<QuerySlotInput*>(
            irp->AssociatedIrp.SystemBuffer);
        auto* slot = findSlot(
            ext, in->slot,
            static_cast<pluginrack::driver::Direction>(in->direction));
        if (slot == nullptr || slot->ringBase == nullptr) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, slot->ringBase,
                      sizeof(pluginrack::driver::SharedRingHeader));
        info = sizeof(pluginrack::driver::SharedRingHeader);
        status = STATUS_SUCCESS;
        break;
    }
    default:
        break;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

// Timer callback: bridges the WaveRT cyclic buffer and the shared SPSC
// ring. Render slots: drain the WaveRT buffer into the ring (engine reads).
// Capture slots: drain the ring into the WaveRT buffer (engine wrote).
// Zero-fills capture when the engine is disconnected so external apps
// never stall. All offsets clamped; system-mapped addresses only.
void BridgeTimerCallback(PVOID context) {
    auto* slot = static_cast<EndpointSlot*>(context);
    if (slot == nullptr || slot->ringBase == nullptr ||
        slot->waveBuffer == nullptr || slot->ringFrames == 0 ||
        slot->waveFrames == 0) {
        return;
    }
    auto* h = static_cast<pluginrack::driver::SharedRingHeader*>(
        slot->ringBase);
    auto* payload = reinterpret_cast<float*>(h + 1);
    auto* wave = static_cast<float*>(slot->waveBuffer);

    const std::uint32_t cap = slot->ringFrames;
    std::uint32_t head = h->headFrames % cap;
    std::uint32_t tail = h->tailFrames % cap;

    if (slot->direction ==
        pluginrack::driver::Direction::RenderToEngine) {
        // Copy newest WaveRT frames into the ring; drop oldest on overflow
        // and count the overrun for Settings telemetry.
        const std::uint32_t waveAvail = slot->waveFrames;
        for (std::uint32_t i = 0; i < waveAvail; ++i) {
            const std::uint32_t wpos =
                (slot->wavePosition + i) % slot->waveFrames;
            const std::uint32_t nhead = (head + 1) % cap;
            if (nhead == tail) {
                h->overruns++;
                tail = (tail + 1) % cap;
            }
            payload[head * 2] = wave[wpos * 2];
            payload[head * 2 + 1] = wave[wpos * 2 + 1];
            head = nhead;
        }
        slot->wavePosition = (slot->wavePosition + waveAvail) % slot->waveFrames;
        h->headFrames = head;
        h->tailFrames = tail;
    } else {
        // Copy ring frames into the WaveRT buffer; zero-fill when the
        // engine is disconnected or the ring is empty.
        const bool connected = h->clientConnected != 0;
        for (std::uint32_t i = 0; i < slot->waveFrames; ++i) {
            const std::uint32_t wpos =
                (slot->wavePosition + i) % slot->waveFrames;
            if (connected && head != tail) {
                wave[wpos * 2] = payload[tail * 2];
                wave[wpos * 2 + 1] = payload[tail * 2 + 1];
                tail = (tail + 1) % cap;
            } else {
                wave[wpos * 2] = 0.0f;
                wave[wpos * 2 + 1] = 0.0f;
                if (connected) h->underruns++;
            }
        }
        slot->wavePosition = (slot->wavePosition + slot->waveFrames) % slot->waveFrames;
        h->tailFrames = tail;
    }
}
