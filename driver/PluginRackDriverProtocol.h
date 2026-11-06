// PluginRackDriverProtocol.h — frozen C ABI between the kernel driver and
// PluginRack.exe. Both sides include this file; the version field rejects
// mismatches at connect time instead of reading garbage.
//
// Kernel constraints (enforced by the driver, asserted by tests):
// - Float32 stereo only. The kernel performs no format conversion or DSP.
// - Offsets are validated and clamped against capacity before any copy.
// - The kernel touches only its system-mapped MDL address, never raw
//   user-mode pointers, at DISPATCH_LEVEL.
// - One engine client per slot: IOCTL claim sets clientConnected, a second
//   opener is rejected until release.
#pragma once

#include <cstdint>

namespace pluginrack::driver {

// Bump on any layout change. The engine refuses to claim a slot whose
// version differs and surfaces a Settings warning.
inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::uint32_t kMagic = 0x50525641u; // 'PRVA'

// Matches DriftResampler's default +-0.5%: wider drift disables the stream.
inline constexpr double kMaxDriftRatio = 0.005;

// v1 topology: 2 render + 2 capture stereo pairs, descriptor table sized
// for 8 pairs without a protocol bump.
inline constexpr std::uint32_t kMaxEndpointPairs = 8;
inline constexpr std::uint32_t kRenderEndpointsV1 = 2;
inline constexpr std::uint32_t kCaptureEndpointsV1 = 2;

enum class Direction : std::uint32_t {
    RenderToEngine = 0, // Windows playback endpoint -> VirtualCapture
    EngineToCapture = 1, // VirtualPlayback -> Windows recording endpoint
};

enum class EndpointState : std::uint32_t {
    Stopped = 0,
    Running = 1,
};

// Fixed-size header, cache-line padded head/tail so the engine poll and the
// kernel timer do not share a line. All frame counts are stereo frames
// (one float per channel per frame).
struct SharedRingHeader {
    std::uint32_t magic; // kMagic
    std::uint32_t version; // kProtocolVersion
    std::uint32_t slot; // 0-based endpoint slot within its direction
    std::uint32_t direction; // Direction
    std::uint32_t sampleRate; // active Windows format rate
    std::uint32_t channels; // always 2 in v1
    std::uint32_t capacityFrames; // ring capacity in stereo frames
    volatile std::uint32_t state; // EndpointState
    // Padded atomics: head written by the producer, tail by the consumer.
    // Render slots: kernel timer writes head, engine reads tail.
    // Capture slots: engine writes head, kernel timer reads tail.
    alignas(64) volatile std::uint32_t headFrames;
    alignas(64) volatile std::uint32_t tailFrames;
    volatile std::uint32_t clientConnected; // IOCTL claim flag
    volatile std::uint32_t underruns;
    volatile std::uint32_t overruns;
    std::uint32_t reserved[4];
};

// Payload follows the header: interleaved float32 stereo, capacityFrames
// frames. Total mapping size = sizeof(SharedRingHeader) +
// capacityFrames * 2 * sizeof(float).
inline constexpr std::uint32_t kMaxRingFrames = 4096;

inline bool validateHeader(const SharedRingHeader& h) noexcept {
    if (h.magic != kMagic) return false;
    if (h.version != kProtocolVersion) return false;
    if (h.channels != 2) return false;
    if (h.sampleRate == 0) return false;
    if (h.capacityFrames == 0 || h.capacityFrames > kMaxRingFrames)
        return false;
    const auto dir = static_cast<Direction>(h.direction);
    return dir == Direction::RenderToEngine ||
           dir == Direction::EngineToCapture;
}

// Clamp a [head, tail) window into [0, capacity): returns frames available.
inline std::uint32_t clampAvailable(std::uint32_t head, std::uint32_t tail,
                                    std::uint32_t capacity) noexcept {
    if (capacity == 0) return 0;
    head %= capacity;
    tail %= capacity;
    return head >= tail ? head - tail : capacity - tail + head;
}

} // namespace pluginrack::driver
