#pragma once

// Shared-memory payload layouts for the plugin host IPC, kept free of JUCE so
// the core test binary can cover them (the same reason NamedSection.h and
// NamedEvent.h are JUCE-free). PluginHostIpc.h includes this header and adds
// the juce::String helpers.
//
// All structures here live in a named section created by the app and opened by
// the child process. They are only ever accessed through the accessors below;
// no code memcpy's them, because the mapping address differs per process.
//
// Threading model, per ring: single producer, single consumer. The producer
// writes the slot first and only then publishes the new head with release
// ordering; the consumer reads the head with acquire ordering before touching
// the slot. That is why the rings need no even/odd torn-read marker (the audio
// block still uses one: several writers-slot fields there are published as a
// group, see SharedAudioBlock::blockSequence).
//
// On x64 the atomic<unsigned long long> members are lock-free and a zeroed
// mapping is a valid initial state, which is what the app relies on when it
// creates the section.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pluginHostIpc {

// --- Parameter ring -------------------------------------------------------

constexpr std::uint32_t kParamFlagGestureActive = 1u << 0; // live edit in progress
constexpr std::uint32_t kParamFlagGestureEnd = 1u << 1;    // last message of a gesture

struct ParamMsg {
    std::uint32_t id = 0;
    // Normalised 0..1, the domain AudioProcessorParameter reports. Carrying an
    // already-normalised value keeps per-plugin conversion out of the IPC.
    float value = 0.0f;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
    // Monotonic per-ring counters: a gap tells the receiver a message was
    // dropped (ring full) and that a full state resync is needed.
    std::uint64_t seq = 0;
};

constexpr int kParamRingSlots = 512; // power of two
static_assert((kParamRingSlots & (kParamRingSlots - 1)) == 0, "ring size must be a power of two");

struct ParamRing {
    std::atomic<std::uint64_t> head{0}; // producer publishes here
    std::atomic<std::uint64_t> tail{0}; // consumer publishes here
    std::atomic<std::uint64_t> producerSeq{0};
    std::atomic<std::uint64_t> consumerSeq{0};
    ParamMsg slots[kParamRingSlots];
};

struct ParamSection {
    ParamRing toHost; // editor/app → child
    ParamRing toApp;  // child → editor/app (phase 2)
};

inline bool paramRingPush(ParamRing& ring, ParamMsg msg) {
    const auto head = ring.head.load(std::memory_order_relaxed);
    const auto tail = ring.tail.load(std::memory_order_acquire);
    if (head - tail >= static_cast<std::uint64_t>(kParamRingSlots)) {
        // Full. Skipping the sequence number is deliberate: the receiver sees
        // the gap and asks for a full state resync instead of silently
        // applying a stale parameter value.
        ring.producerSeq.store(head + 1, std::memory_order_release);
        return false;
    }
    msg.seq = ring.producerSeq.load(std::memory_order_relaxed) + 1;
    ring.slots[head % static_cast<std::uint64_t>(kParamRingSlots)] = msg;
    ring.producerSeq.store(msg.seq, std::memory_order_release);
    ring.head.store(head + 1, std::memory_order_release);
    return true;
}

inline bool paramRingPop(ParamRing& ring, ParamMsg& msg) {
    const auto tail = ring.tail.load(std::memory_order_relaxed);
    if (tail == ring.head.load(std::memory_order_acquire)) return false;
    msg = ring.slots[tail % static_cast<std::uint64_t>(kParamRingSlots)];
    ring.consumerSeq.store(msg.seq, std::memory_order_release);
    ring.tail.store(tail + 1, std::memory_order_release);
    return true;
}

// True when the producer published sequence numbers the consumer never saw.
// Call this after draining the ring: a message dropped because the ring was
// full still consumed a sequence number, so the receiver can detect the loss
// and request a full state resync instead of applying a stale value.
inline std::uint64_t paramRingLostMessages(const ParamRing& ring) {
    const auto produced = ring.producerSeq.load(std::memory_order_acquire);
    const auto consumed = ring.consumerSeq.load(std::memory_order_acquire);
    return produced > consumed ? produced - consumed : 0;
}

// True when a sequence number did not follow the previous one.
inline bool paramSeqGapped(std::uint64_t previous, std::uint64_t next) {
    return next > previous + 1;
}

// --- Control channel ------------------------------------------------------

// One request in flight at a time, message/worker threads only. Sized for the
// base64 state cap (PluginHost.cpp's kMaxPluginStateBase64Chars) plus slack;
// sections are pagefile-backed, so the committed cost is the touched pages.
constexpr std::size_t kControlPayloadBytes = 1024 * 1024 + 4096;

enum class ControlOp : std::uint32_t {
    none = 0,
    prepare = 1,
    setState = 2,
    getState = 3,
    getLatency = 4,
    ping = 5
};

enum class ControlStatus : std::uint32_t {
    ok = 0,
    refused = 1, // understood, declined (e.g. state over the cap)
    failed = 2   // could not do it (plugin rejected the state, no instance)
};

// Handshake states for ControlSection::hostReady.
constexpr std::uint32_t kHostReadyPending = 0;
constexpr std::uint32_t kHostReadyLoaded = 1;
constexpr std::uint32_t kHostReadyFailed = 2;

struct ControlSection {
    // The child records its own process id here before it prints HOSTREADY, so
    // the app can name the process in a fault record (and a diagnostic can kill
    // exactly this child) without changing the handshake text.
    std::atomic<std::uint32_t> hostPid{0};
    // 0 = starting, 1 = plugin loaded and every channel wired, 2 = failed.
    // The app polls this for the handshake: ChildProcess::readProcessOutput
    // BLOCKS when the pipe has no data, so polling stdout would deadlock the
    // loading worker. Stdout is read only after the child has exited.
    std::atomic<std::uint32_t> hostReady{0};
    // Same even/odd discipline as the audio block: the writer marks odd while
    // filling op/payload, then marks even and only then publishes requestSeq.
    std::atomic<std::uint64_t> blockSequence{0};
    std::atomic<std::uint64_t> requestSeq{0};
    std::atomic<std::uint64_t> responseSeq{0};
    std::atomic<std::uint64_t> payloadBytes{0};
    std::atomic<std::uint32_t> op{0};
    std::atomic<std::uint32_t> status{0};
    char payload[kControlPayloadBytes] = {};
};

inline void controlBeginWrite(ControlSection& section) {
    section.blockSequence.store(section.blockSequence.load(std::memory_order_relaxed) + 1,
                                std::memory_order_release); // odd
}

inline void controlEndWrite(ControlSection& section) {
    section.blockSequence.store(section.blockSequence.load(std::memory_order_relaxed) + 1,
                                std::memory_order_release); // even
}

// Copies `text` into the section payload. Returns false (and leaves the payload
// untouched) when it would not fit, so an oversized state is a refused request
// rather than a truncated one.
inline bool controlSetPayload(ControlSection& section, const char* text, std::size_t length) {
    if (text == nullptr) return false;
    if (length + 1 > kControlPayloadBytes) return false;
    for (std::size_t i = 0; i < length; ++i) section.payload[i] = text[i];
    section.payload[length] = '\0';
    section.payloadBytes.store(length, std::memory_order_release);
    return true;
}

// True when the payload is NUL-terminated inside the buffer. A child that
// writes a length without a terminator must be treated as failed, not parsed.
inline bool controlPayloadTerminated(const ControlSection& section) {
    const auto bytes = section.payloadBytes.load(std::memory_order_acquire);
    return bytes < kControlPayloadBytes && section.payload[bytes] == '\0';
}

inline const char* controlOpName(ControlOp op) {
    switch (op) {
        case ControlOp::prepare: return "prepare";
        case ControlOp::setState: return "setState";
        case ControlOp::getState: return "getState";
        case ControlOp::getLatency: return "getLatency";
        case ControlOp::ping: return "ping";
        case ControlOp::none: break;
    }
    return "none";
}

} // namespace pluginHostIpc
