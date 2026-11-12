#pragma once

// Shared memory layout between PluginRack and the restricted plugin host.
// One stereo block of floats, plus a header that carries the current block
// size and a sequence counter. The host runs the plugin on the copied audio
// and flips the sequence counter when the block is done, so the app can tell
// a live host from a stuck one.
//
// Wake-ups travel over two named auto-reset events derived from the channel
// name: the app signals "request" after publishing a block, and the host
// signals "response" after acking it. Events wake the waiter immediately, so
// the hand-off does not depend on the system timer resolution (Windows 11
// drops a background process's 1 ms request to the default 15.6 ms tick).
//
// Seamlessness rule: submitAudioBlock blocks until the host has acked the
// previous block, so the single shared block can never be overwritten
// while the host is still producing from it. If the host does not answer
// within the budget, submit fails and the caller (PluginHost::processBlock)
// falls back to rendering the in-process instance — but it must NOT have
// mutated the caller's buffer, and it must NOT have consumed the block.
// That is why the app never submits the live output block directly: it
// copies the range into a per-slot scratch first (see PluginHost), so a
// fallback leaves the audio intact and the next block can safely retry the
// host.

#include <JuceHeader.h>

#include <cstdint>

namespace pluginHostIpc {

constexpr int kMaxBlockFrames = 8192;

struct SharedAudioBlock {
    volatile std::uint64_t sequence = 0;         // app request counter
    volatile std::uint64_t responseSequence = 0; // host ack == sequence it answered
    volatile std::uint64_t writeSequence = 0;    // app payload version
    volatile std::uint64_t readSequence = 0;     // host last-read version
    volatile std::uint64_t ackSequence = 0;      // host last-processed version
    volatile std::uint64_t blockSequence = 0;    // even=stable, odd=writer active
    volatile std::uint64_t lastSequence = 0;     // host's last seen sequence
    int numFrames = 0;
    int bufferSize = 0;
    double sampleRate = 0.0;
    float interleave[kMaxBlockFrames * 2] = {};
};

inline juce::String sharedNameFor(const juce::String& pluginPath) {
    // A short base name: lower-case hex of a simple hash, no filesystem
    // characters, so it is safe as a Win32 shared-memory name. Callers must
    // append a per-instance discriminator: a scene can host the same plugin
    // file in more than one slot.
    juce::String hex = juce::String::toHexString(pluginPath.hashCode());
    return "PluginRackHost-" + hex;
}

// Both processes derive the event names from the channel name that is already
// passed on the host command line, so a name can never drift between them.
inline juce::String requestEventNameFor(const juce::String& channelName) {
    return channelName + "-req";
}

inline juce::String responseEventNameFor(const juce::String& channelName) {
    return channelName + "-rsp";
}

} // namespace pluginHostIpc
