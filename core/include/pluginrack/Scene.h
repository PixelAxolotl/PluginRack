#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pluginrack/EqEngine.h"
#include "pluginrack/DynamicsEffect.h"
#include "pluginrack/MultibandEffect.h"
#include "pluginrack/DeEsserEffect.h"
#include "pluginrack/LimiterEffect.h"
#include "pluginrack/FeedbackSuppressorEffect.h"
#include "pluginrack/Types.h"

namespace pluginrack {

// Bumped only when an older file must be read differently. The writer stamps
// this value, the reader compares against it, and the validator rejects
// anything newer, so the number lives in one place.
inline constexpr int kSchemaVersion = 8;

// Where an insert's side-chain input is fed from. None keeps the pre-v8
// behaviour (the plugin's side-chain bus is disabled). Channel and Output
// reference a live strip by id, Bus references one of the scene's AuxBus
// entries, which mix a set of channels with their own gain.
enum class SideChainSource { None, Channel, Output, Bus };

// Which point of the source strip is tapped. PreInsert is the raw input before
// the strip's own chain runs; PostFader is the fully processed signal after
// gain/pan. Both are saved so a session reopens with the same trigger.
enum class SideChainTap { PreInsert, PostFader };

struct InsertSlot {
    std::uint32_t id = 0;
    std::string pluginPath;
    std::string pluginName;
    // Which class inside a multi-class VST3 bundle this slot hosts. A bundle
    // can expose several processors, and the path alone cannot say which one
    // was picked. 0 means "not recorded" (scenes written before this existed,
    // or a slot filled through the file browser), and the loader falls back to
    // the bundle's first class.
    int pluginUniqueId = 0;
    bool bypassed = false;
    // Opaque base64 blob of the plugin's own state (parameters, programs,
    // samples). The core engine never interprets it; the host layer captures
    // it on save and feeds it back to the plugin when the slot is rebuilt.
    // Empty means the plugin keeps its defaults.
    std::string pluginState;
    // Side-chain routing (schema 8). Defaults reproduce the old behaviour, so
    // scenes written before v8 load with the side-chain bus disabled.
    SideChainSource sideChainSource = SideChainSource::None;
    std::uint32_t sideChainSourceId = 0;
    SideChainTap sideChainTap = SideChainTap::PostFader;
    float sideChainGainDb = 0.0f;

    bool operator==(const InsertSlot&) const = default;
};

// A dedicated side-chain mix bus. Channels listed in sourceChannelIds are
// summed into this bus, scaled by gainDb, and any insert can select it as its
// side-chain source. Aux buses are scene state; their audio buffers are
// runtime-only and live in the engine.
struct AuxBus {
    std::uint32_t id = 0;
    std::string name;
    float gainDb = 0.0f;
    std::vector<std::uint32_t> sourceChannelIds;

    bool operator==(const AuxBus&) const = default;
};

enum class ChainItemType { Eq, Dynamics, Insert, Multiband, DeEsser, Limiter, FeedbackSuppressor };

struct ChainItem {
    ChainItemType type = ChainItemType::Insert;
    std::uint32_t id = 0;

    bool operator==(const ChainItem&) const = default;
};

// Everything a channel and an output share: identity, level, inserts, the
// effect chain and the EQ. Both strip kinds render through the same
// MixerEngine::StripDsp, so a field that changes the audio path belongs here
// and reaches both at once.
struct Strip {
    std::uint32_t id = 0;
    std::string name;
    ChannelLayout layout = ChannelLayout::Stereo;
    float gainDb = 0.0f;
    bool muted = false;
    std::vector<InsertSlot> inserts;
    std::vector<DynamicsEffect> effects = defaultDynamicsEffects();
    std::vector<MultibandEffect> multibandEffects;
    std::vector<DeEsserEffect> deEsserEffects;
    std::vector<LimiterEffect> limiterEffects;
    std::vector<FeedbackSuppressorEffect> feedbackSuppressorEffects;
    float multibandRtaOffsetDb = 0.0f;
    int multibandRtaWeighting = 0;
    float deEsserRtaOffsetDb = 0.0f;
    int deEsserRtaWeighting = 0;
    std::vector<ChainItem> chain;
    bool eqEnabled = false;
    std::vector<EqBand> eqBands = defaultEqBands();
    // Post-EQ delay in milliseconds, 0..EqEngine::kMaxDelayMs; 0 keeps the
    // chain undelayed. Like the bands it only runs while eqEnabled.
    float eqDelayMs = 0.0f;
    // Strip identity hue in degrees; 0 means "assign on load" so old scenes
    // still get a colour.
    float colourHue = 0.0f;
    // Polarity flip applied after the EQ runs (inverts phase).
    bool eqPhaseFlip = false;
    // RTA display offset in dB, shown relative to the default view (0 ==
    // default). Display only: it never touches the audio path, but it is
    // saved so the analyzer looks the same when the scene comes back.
    float eqRtaOffsetDb = 0.0f;
    // Analyzer weighting: 0 = Z (flat), 1 = A, 2 = C. Display only.
    int eqRtaWeighting = 0;

    bool operator==(const Strip&) const = default;
};

struct Channel : Strip {
    // Inputs default to mono; a scene can widen them.
    Channel() { layout = ChannelLayout::Mono; }

    SourceKind sourceKind = SourceKind::Hardware;
    std::string sourceDeviceId;
    std::vector<int> inputChannels{0};
    float trimDb = 0.0f;
    float pan = 0.0f;
    bool soloed = false;

    bool operator==(const Channel&) const = default;
};

struct Output : Strip {
    OutputKind kind = OutputKind::Physical;
    std::string deviceId;
    bool flipped = false;

    bool operator==(const Output&) const = default;
};

struct Route {
    std::uint32_t channelId = 0;
    std::uint32_t outputId = 0;
    float sendGainDb = 0.0f;
    // Right-side send level when it differs from the left. Absent means "same
    // as sendGainDb", so scenes written before the split restore a matched
    // pair. Only written to JSON when the two sides actually differ. The live
    // engine and the offline MixerEngine both apply the sides independently.
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

// How the Rack view keeps insert-card previews fresh for plugins that are
// not docked in the center bay. Selected keeps today's behavior (only the
// focused card is live); Rotate cycles background cards one at a time;
// AllLive keeps a hidden editor per insert and refreshes each at low rate.
enum class RackPreviewMode { Selected, Rotate, AllLive };

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
    // Scene v5 graduated the two bounded low-latency switches in the engine's
    // own defaults (a fresh session runs with them on), but the schema stays
    // conservative: an absent key means off, and pre-v5 scenes never opt in.
    bool lowLatencyInput = false;
    bool preferExclusive = false;
    bool lowLatencyOutputs = false;
    bool minimalClockFifo = false;
    bool fullDuplexInput = false;
    bool autoReloadDevices = false;
    std::vector<WatchedDevice> watchedDevices;
    // Scene v6 audio-pipeline hardening. Optional and off by default, so a
    // pre-v6 save keeps its exact behaviour until the user opts in. Once a
    // scene is saved with them on, the switch travels with the session.
    bool slicedRender = false;
    bool nonFiniteScrub = false;
    bool healthReload = false;
    bool ladderRecovery = false;
    // Scene v7: opt-in cap for JUCE's WASAPI channel-count probe. Off by
    // default; pre-v7 saves never turn it on.
    bool wasapiProbeCapEnabled = false;
    int wasapiProbeCapChannels = 8;
    // Health thresholds travel with the scene; defaults are the proposed
    // values when a save predates v6.
    double healthYellowUnderrunsPerMinute = 1.0;
    double healthRedUnderrunsPerMinute = 10.0;
    double healthPinnedToEdgeMs = 5000.0;
    // Refresh rate of the Rack view's insert thumbnails, in frames per
    // second. Not an audio stream option, but it saves and loads with the
    // rest so a session comes back looking the way it was left.
    int rackThumbnailFps = 15;
    // Which inserts keep live previews. Display only, saved like the rate.
    RackPreviewMode rackPreviewMode = RackPreviewMode::Selected;

    bool operator==(const AudioSettings&) const = default;
};

struct Scene {
    int schemaVersion = kSchemaVersion;
    std::string name;
    double sampleRate = 48000;
    int bufferFrames = 256;
    std::vector<Channel> channels;
    std::vector<Output> outputs;
    std::vector<Route> routes;
    // Dedicated side-chain mix buses (schema 8). Absent in older scenes, which
    // therefore load with no aux buses.
    std::vector<AuxBus> auxBuses;
    // Absent in scenes written before schema 4; loading one then leaves the
    // live tuning alone instead of resetting it to the defaults.
    std::optional<AudioSettings> audio;

    bool operator==(const Scene&) const = default;
};

} // namespace pluginrack
