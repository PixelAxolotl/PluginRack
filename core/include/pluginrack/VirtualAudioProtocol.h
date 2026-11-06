#pragma once

#include <cstdint>
#include <string>

namespace pluginrack::virt {

// Magic + version for PluginRackDriverProtocol.h. The kernel driver and the
// engine compare these before touching the shared ring; a mismatch disables
// the stream with a Settings warning instead of reading garbage.
inline constexpr std::uint32_t kMagic = 0x50525641u; // 'PRVA'
inline constexpr std::uint32_t kProtocolVersion = 1;

// DriftResampler defaults to +-0.5% correction: anything wider must disable
// the virtual stream per the rate-mismatch policy, never stretch audio.
inline constexpr double kMaxDriftRatio = 0.005;

enum class Direction : std::uint32_t {
    RenderToEngine = 0, // Windows playback endpoint -> VirtualCapture
    EngineToCapture = 1, // VirtualPlayback -> Windows recording endpoint
};

struct EndpointDescriptor {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t slot = 0;
    std::uint32_t direction = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
};

// Float32 stereo only: the kernel performs no format conversion or DSP.
inline bool validate(const EndpointDescriptor& desc) noexcept {
    if (desc.magic != kMagic) return false;
    if (desc.version != kProtocolVersion) return false;
    if (desc.channels != 2) return false;
    if (desc.sampleRate == 0) return false;
    const auto dir = static_cast<Direction>(desc.direction);
    return dir == Direction::RenderToEngine ||
           dir == Direction::EngineToCapture;
}

// Stable logical IDs bound to driver slot indices, never to volatile
// Windows MMDevice GUIDs, so scenes stay portable across machines.
inline std::string makeVirtualInputId(int slot) {
    return "pluginrack://vaio/in/" + std::to_string(slot);
}

inline std::string makeVirtualOutputId(int slot) {
    return "pluginrack://vaio/out/" + std::to_string(slot);
}

inline bool isVirtualEndpointId(const std::string& id) {
    return id.rfind("pluginrack://vaio/", 0) == 0;
}

// Returns the slot index, or -1 when the id is not a virtual endpoint id.
inline int parseVirtualSlot(const std::string& id) {
    constexpr const char* kIn = "pluginrack://vaio/in/";
    constexpr const char* kOut = "pluginrack://vaio/out/";
    const char* tail = nullptr;
    if (id.rfind(kIn, 0) == 0) {
        tail = id.c_str() + sizeof("pluginrack://vaio/in/") - 1;
    } else if (id.rfind(kOut, 0) == 0) {
        tail = id.c_str() + sizeof("pluginrack://vaio/out/") - 1;
    } else {
        return -1;
    }
    if (*tail == '\0') return -1;
    int slot = 0;
    for (const char* p = tail; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return -1;
        slot = slot * 10 + (*p - '0');
    }
    return slot;
}

// True when the endpoint rate is close enough for DriftResampler to track.
inline bool isRateCompatible(double engineRate, double endpointRate) noexcept {
    if (!(engineRate > 0.0) || !(endpointRate > 0.0)) return false;
    const double ratio = endpointRate / engineRate;
    const double drift = ratio >= 1.0 ? ratio - 1.0 : 1.0 - ratio;
    return drift <= kMaxDriftRatio;
}

} // namespace pluginrack::virt
