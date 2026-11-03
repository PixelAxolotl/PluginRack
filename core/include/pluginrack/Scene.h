#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pluginrack/EqEngine.h"
#include "pluginrack/DynamicsEffect.h"
#include "pluginrack/Types.h"

namespace pluginrack {

struct InsertSlot {
    std::uint32_t id = 0;
    std::string pluginPath;
    std::string pluginName;
    bool bypassed = false;
    // Opaque base64 blob of the plugin's own state (parameters, programs,
    // samples). The core engine never interprets it; the host layer captures
    // it on save and feeds it back to the plugin when the slot is rebuilt.
    // Empty means the plugin keeps its defaults.
    std::string pluginState;

    bool operator==(const InsertSlot&) const = default;
};

enum class ChainItemType { Eq, Dynamics, Insert };

struct ChainItem {
    ChainItemType type = ChainItemType::Insert;
    std::uint32_t id = 0;

    bool operator==(const ChainItem&) const = default;
};

struct Channel {
    std::uint32_t id = 0;
    std::string name;
    SourceKind sourceKind = SourceKind::Hardware;
    std::string sourceDeviceId;
    ChannelLayout layout = ChannelLayout::Mono;
    std::vector<int> inputChannels{0};
    float gainDb = 0.0f;
    float trimDb = 0.0f;
    float pan = 0.0f;
    bool muted = false;
    bool soloed = false;
    std::vector<InsertSlot> inserts;
    std::vector<DynamicsEffect> effects = defaultDynamicsEffects();
    std::vector<ChainItem> chain;
    bool eqEnabled = false;
    std::vector<EqBand> eqBands = defaultEqBands();
    // Post-EQ delay in milliseconds, 0..EqEngine::kMaxDelayMs; 0 keeps the
    // chain undelayed. Like the bands it only runs while eqEnabled.
    float eqDelayMs = 0.0f;
    // RTA display offset in dB, shown relative to the default view (0 ==
    // default). Display only: it never touches the audio path, but it is
    // saved so the analyzer looks the same when the scene comes back.
    float eqRtaOffsetDb = 0.0f;
    // Analyzer weighting: 0 = Z (flat), 1 = A, 2 = C. Display only.
    int eqRtaWeighting = 0;

    bool operator==(const Channel&) const = default;
};

struct Output {
    std::uint32_t id = 0;
    std::string name;
    OutputKind kind = OutputKind::Physical;
    std::string deviceId;
    ChannelLayout layout = ChannelLayout::Stereo;
    bool flipped = false;
    float gainDb = 0.0f;
    bool muted = false;
    std::vector<InsertSlot> inserts;
    std::vector<DynamicsEffect> effects = defaultDynamicsEffects();
    std::vector<ChainItem> chain;
    bool eqEnabled = false;
    std::vector<EqBand> eqBands = defaultEqBands();
    // Post-EQ delay in milliseconds, 0..EqEngine::kMaxDelayMs; 0 keeps the
    // chain undelayed. Like the bands it only runs while eqEnabled.
    float eqDelayMs = 0.0f;
    // RTA display offset in dB, shown relative to the default view (0 ==
    // default). Display only: it never touches the audio path.
    float eqRtaOffsetDb = 0.0f;
    // Analyzer weighting: 0 = Z (flat), 1 = A, 2 = C. Display only.
    int eqRtaWeighting = 0;

    bool operator==(const Output&) const = default;
};

struct Route {
    std::uint32_t channelId = 0;
    std::uint32_t outputId = 0;
    float sendGainDb = 0.0f;
    // Right-side send level when it differs from the left. Absent means "same
    // as sendGainDb", so scenes written before the split (and the core engine,
    // which only has one send level) restore a matched pair. Only written to
    // JSON when the two sides actually differ.
    std::optional<float> sendGainDbR;

    bool operator==(const Route&) const = default;
};

// One stream the Settings page watches for auto-reload. Identified by the
// strip that owns it, so renaming a strip does not lose the selection.
struct WatchedDevice {
    bool isInput = false;
    std::uint32_t stripId = 0;

    bool operator==(const WatchedDevice&) const = default;
};

// The audio engine settings a session is tuned with: buffer sizes, the
// low-latency trade-offs, and whether watched devices reopen themselves after
// an error. Saved with the scene so a load restores the same running setup.
struct AudioSettings {
    int inputBuffer = 0;
    int outputBuffer = 0;
    // Beta latency tuning is an explicit opt-in. This marker lets older
    // scenes that happened to contain tuning keys migrate back to the stable
    // defaults instead of silently enabling the aggressive modes on startup.
    bool latencyTuningOptIn = false;
    bool lowLatencyInput = false;
    bool preferExclusive = false;
    bool lowLatencyOutputs = false;
    bool minimalClockFifo = false;
    bool fullDuplexInput = false;
    bool autoReloadDevices = false;
    std::vector<WatchedDevice> watchedDevices;
    // Refresh rate of the Rack view's insert thumbnails, in frames per
    // second. Not an audio stream option, but it saves and loads with the
    // rest so a session comes back looking the way it was left.
    int rackThumbnailFps = 15;

    bool operator==(const AudioSettings&) const = default;
};

struct Scene {
    int schemaVersion = 4;
    std::string name;
    double sampleRate = 48000;
    int bufferFrames = 256;
    std::vector<Channel> channels;
    std::vector<Output> outputs;
    std::vector<Route> routes;
    // Absent in scenes written before schema 4; loading one then leaves the
    // live tuning alone instead of resetting it to the defaults.
    std::optional<AudioSettings> audio;

    bool operator==(const Scene&) const = default;
};

} // namespace pluginrack
