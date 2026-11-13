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
// within the budget, the caller (PluginHost::processBlock) leaves the block
// DRY for that callback. It must not render the in-process instance instead:
// that instance has had no input all session, so switching to it would be a
// silent audible discontinuity with stale meters (see the fault containment
// spec, docs/superpowers/specs/2026-11-13-fault-containment-ipc.md §3.2).
// The caller must NOT have mutated its live buffer and must NOT have consumed
// the block, so it copies the range into a per-slot scratch first.

#include <JuceHeader.h>

#include <cstdint>

#include "PluginIpcLayout.h"

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
    // Written by the host before it bumps responseSequence, so a value read
    // after a matching ack belongs to the block that was just processed.
    float peakInL = 0.0f;
    float peakInR = 0.0f;
    float peakOutL = 0.0f;
    float peakOutR = 0.0f;
    std::int32_t latencySamples = 0;
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

// Control channel wake-ups. Kept separate from the audio pair so a blocked
// state request can never delay an audio callback (and vice versa). Parameter
// traffic has no event of its own: the app flushes its ring before publishing
// a block, and the host flushes its ring before acking one.
inline juce::String controlRequestEventNameFor(const juce::String& channelName) {
    return channelName + "-ctl-req";
}

inline juce::String controlResponseEventNameFor(const juce::String& channelName) {
    return channelName + "-ctl-rsp";
}

} // namespace pluginHostIpc
