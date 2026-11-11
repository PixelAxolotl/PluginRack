#pragma once

// Shared memory layout between PluginRack and the sandboxed plugin host.
// One stereo block of floats, plus a header that carries the current block
// size and a sequence counter. The host runs the plugin on the copied audio
// and flips the sequence counter when the block is done, so the app can tell
// a live host from a stuck one.
//
// Fail-closed rule: submitAudioBlock never blocks behind the host, and when
// the host does not answer within the budget the caller (PluginHost::
// processBlock) passes dry audio for that block — never an in-process
// render. The app never submits the live output block directly: it copies
// the range into a per-slot scratch first (see PluginHost), so a dead host
// leaves the audio intact and the next block can safely retry.

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>

namespace pluginHostIpc {

constexpr int kMaxBlockFrames = 8192;

struct SharedAudioBlock {
    // PR-12: acquire/release atomics, not volatile. Lock-free on x64 MSVC;
    // the static_assert below fails the build on targets where it is not.
    std::atomic<std::uint64_t> sequence{0};         // app request counter
    std::atomic<std::uint64_t> responseSequence{0}; // host ack == sequence it answered
    std::atomic<std::uint64_t> writeSequence{0};    // app payload version
    std::atomic<std::uint64_t> readSequence{0};     // host last-read version
    std::atomic<std::uint64_t> ackSequence{0};      // host last-processed version
    std::atomic<std::uint64_t> blockSequence{0};    // even=stable, odd=writer active
    std::atomic<std::uint64_t> lastSequence{0};     // host's last seen sequence
    int numFrames = 0;
    int bufferSize = 0;
    double sampleRate = 0.0;
    float interleave[kMaxBlockFrames * 2] = {};
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "SharedAudioBlock counters must be lock-free atomics");

// PR-SEC-004: per-launch random section names. The old name was a 32-bit hash
// of the plugin path, so two inserts of the same plugin shared one mapping
// and any same-user process could pre-create or open it. Callers pass the
// name to the child over the launch channel (never derive it from the path),
// and the parent rejects ERROR_ALREADY_EXISTS.
inline juce::String makeRandomSharedName() {
    juce::Random rng(juce::Time::currentTimeMillis()
                     ^ static_cast<juce::int64>(juce::SystemStats::getMemorySizeInMegabytes()));
    juce::String hex;
    for (int i = 0; i < 4; ++i)
        hex += juce::String::toHexString(rng.nextInt());
    // Explicit session-local prefix: identical on both ends, creatable
    // without SeCreateGlobalPrivilege, and openable from a low-integrity
    // child (verified: bare and Local\ names resolve the same object).
    return "Local\\PluginRackHost-" + hex;
}

inline juce::String sharedNameFor(const juce::String& pluginPath) {
    // Deprecated: kept for the --plugin-host child launched by older builds.
    // New launches use makeRandomSharedName() and pass the name explicitly.
    juce::String hex = juce::String::toHexString(pluginPath.hashCode());
    return "PluginRackHost-" + hex;
}

} // namespace pluginHostIpc
