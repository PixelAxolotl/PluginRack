#pragma once

// Shared memory layout between PluginRack and the restricted plugin host.
// One stereo block of floats, plus a header that carries the current block
// size and a sequence counter. The host runs the plugin on the copied audio
// and flips the sequence counter when the block is done, so the app can tell
// a live host from a stuck one.

#include <JuceHeader.h>

#include <cstdint>

namespace pluginHostIpc {

constexpr int kMaxBlockFrames = 8192;

struct SharedAudioBlock {
    volatile std::uint64_t sequence = 0;
    int numFrames = 0;
    float interleave[kMaxBlockFrames * 2] = {};
};

inline juce::String sharedNameFor(const juce::String& pluginPath) {
    // A short per-plugin name: lower-case hex of a simple hash, no filesystem
    // characters, so it is safe as a Win32 shared-memory name.
    juce::String hex = juce::String::toHexString(pluginPath.hashCode());
    return "PluginRackHost-" + hex;
}

} // namespace pluginHostIpc
