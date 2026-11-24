#include "pluginrack/SceneCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace pluginrack {

void to_json(nlohmann::json& j, SourceKind kind) {
    switch (kind) {
        case SourceKind::Hardware: j = "Hardware"; break;
        case SourceKind::SystemLoopback: j = "SystemLoopback"; break;
        case SourceKind::VirtualInput: j = "VirtualInput"; break;
    }
}

void from_json(const nlohmann::json& j, SourceKind& kind) {
    const std::string name = j.get<std::string>();
    if (name == "Hardware") {
        kind = SourceKind::Hardware;
    } else if (name == "SystemLoopback") {
        kind = SourceKind::SystemLoopback;
    } else if (name == "VirtualInput") {
        kind = SourceKind::VirtualInput;
    } else {
        throw nlohmann::json::other_error::create(502, "unknown SourceKind '" + name + "'", &j);
    }
}

void to_json(nlohmann::json& j, OutputKind kind) {
    switch (kind) {
        case OutputKind::Physical: j = "Physical"; break;
        case OutputKind::VirtualOutput: j = "VirtualOutput"; break;
    }
}

void from_json(const nlohmann::json& j, OutputKind& kind) {
    const std::string name = j.get<std::string>();
    if (name == "Physical") {
        kind = OutputKind::Physical;
    } else if (name == "VirtualOutput") {
        kind = OutputKind::VirtualOutput;
    } else {
        throw nlohmann::json::other_error::create(502, "unknown OutputKind '" + name + "'", &j);
    }
}

void to_json(nlohmann::json& j, ChannelLayout layout) {
    switch (layout) {
        case ChannelLayout::Mono: j = "Mono"; break;
        case ChannelLayout::Stereo: j = "Stereo"; break;
    }
}

void from_json(const nlohmann::json& j, ChannelLayout& layout) {
    const std::string name = j.get<std::string>();
    if (name == "Mono") {
        layout = ChannelLayout::Mono;
    } else if (name == "Stereo") {
        layout = ChannelLayout::Stereo;
    } else {
        throw nlohmann::json::other_error::create(502, "unknown ChannelLayout '" + name + "'", &j);
    }
}

void to_json(nlohmann::json& j, SideChainSource source) {
    switch (source) {
        case SideChainSource::None: j = "None"; break;
        case SideChainSource::Channel: j = "Channel"; break;
        case SideChainSource::Output: j = "Output"; break;
        case SideChainSource::Bus: j = "Bus"; break;
    }
}

void from_json(const nlohmann::json& j, SideChainSource& source) {
    const std::string name = j.get<std::string>();
    if (name == "None") {
        source = SideChainSource::None;
    } else if (name == "Channel") {
        source = SideChainSource::Channel;
    } else if (name == "Output") {
        source = SideChainSource::Output;
    } else if (name == "Bus") {
        source = SideChainSource::Bus;
    } else {
        throw nlohmann::json::other_error::create(504, "unknown SideChainSource '" + name + "'", &j);
    }
}

void to_json(nlohmann::json& j, SideChainTap tap) {
    switch (tap) {
        case SideChainTap::PreInsert: j = "PreInsert"; break;
        case SideChainTap::PostFader: j = "PostFader"; break;
    }
}

void from_json(const nlohmann::json& j, SideChainTap& tap) {
    const std::string name = j.get<std::string>();
    if (name == "PreInsert") {
        tap = SideChainTap::PreInsert;
    } else if (name == "PostFader") {
        tap = SideChainTap::PostFader;
    } else {
        throw nlohmann::json::other_error::create(505, "unknown SideChainTap '" + name + "'", &j);
    }
}

void to_json(nlohmann::json& j, const AuxBus& bus) {
    j = nlohmann::json{
        {"id", bus.id},
        {"name", bus.name},
        {"gainDb", bus.gainDb},
        {"sourceChannelIds", bus.sourceChannelIds},
    };
}

void from_json(const nlohmann::json& j, AuxBus& bus) {
    bus = AuxBus{};
    j.at("id").get_to(bus.id);
    bus.name = j.value("name", std::string{});
    bus.gainDb = j.value("gainDb", 0.0f);
    bus.sourceChannelIds = j.value("sourceChannelIds", std::vector<std::uint32_t>{});
}

void to_json(nlohmann::json& j, EqType type) {
    switch (type) {
        case EqType::HighPass: j = "HighPass"; break;
        case EqType::LowShelf: j = "LowShelf"; break;
        case EqType::Peak: j = "Peak"; break;
        case EqType::HighShelf: j = "HighShelf"; break;
        case EqType::LowPass: j = "LowPass"; break;
    }
}

void from_json(const nlohmann::json& j, EqType& type) {
    const std::string name = j.get<std::string>();
    if (name == "HighPass") {
        type = EqType::HighPass;
    } else if (name == "LowShelf") {
        type = EqType::LowShelf;
    } else if (name == "Peak") {
        type = EqType::Peak;
    } else if (name == "HighShelf") {
        type = EqType::HighShelf;
    } else if (name == "LowPass") {
        type = EqType::LowPass;
    } else {
        throw nlohmann::json::other_error::create(502, "unknown EqType '" + name + "'", &j);
    }
}

void to_json(nlohmann::json& j, const EqBand& band) {
    j = nlohmann::json{
        {"type", band.type},
        {"freqHz", band.freqHz},
        {"gainDb", band.gainDb},
        {"q", band.q},
        {"enabled", band.enabled},
        {"solo", band.solo},
    };
}

void from_json(const nlohmann::json& j, EqBand& band) {
    band = EqBand{};
    j.at("type").get_to(band.type);
    band.freqHz = j.value("freqHz", 1000.0f);
    band.gainDb = j.value("gainDb", 0.0f);
    band.q = j.value("q", 0.707f);
    band.enabled = j.value("enabled", true);
    band.solo = j.value("solo", false);
}

void to_json(nlohmann::json& j, MultibandMode mode) {
    switch (mode) {
        case MultibandMode::Compress: j = "Compress"; break;
        case MultibandMode::Expand: j = "Expand"; break;
    }
}

void from_json(const nlohmann::json& j, MultibandMode& mode) {
    const auto name = j.get<std::string>();
    if (name == "Compress") mode = MultibandMode::Compress;
    else if (name == "Expand") mode = MultibandMode::Expand;
    else throw nlohmann::json::other_error::create(502, "unknown MultibandMode '" + name + "'", &j);
}

void to_json(nlohmann::json& j, const MultibandBand& band) {
    j = nlohmann::json{{"bypassed", band.bypassed}, {"soloed", band.soloed}, {"enabled", band.enabled},
                       {"mode", band.mode}, {"thresholdDb", band.thresholdDb}, {"ratio", band.ratio},
                       {"attackMs", band.attackMs}, {"releaseMs", band.releaseMs}, {"makeupDb", band.makeupDb},
                       {"inputGainDb", band.inputGainDb}, {"kneeDb", band.kneeDb}};
}

void from_json(const nlohmann::json& j, MultibandBand& band) {
    band = MultibandBand{};
    band.bypassed = j.value("bypassed", false);
    band.soloed = j.value("soloed", false);
    band.enabled = j.value("enabled", true);
    if (j.contains("mode")) j.at("mode").get_to(band.mode);
    band.thresholdDb = j.value("thresholdDb", band.thresholdDb);
    band.ratio = j.value("ratio", band.ratio);
    band.attackMs = j.value("attackMs", band.attackMs);
    band.releaseMs = j.value("releaseMs", band.releaseMs);
    band.makeupDb = j.value("makeupDb", band.makeupDb);
    band.inputGainDb = j.value("inputGainDb", band.inputGainDb);
    band.kneeDb = j.value("kneeDb", band.kneeDb);
}

void to_json(nlohmann::json& j, const MultibandEffect& effect) {
    j = nlohmann::json{{"id", effect.id}, {"bypassed", effect.bypassed}, {"autoMakeup", effect.autoMakeup},
                       {"bands", effect.bands}, {"crossovers", effect.crossovers}};
}

void from_json(const nlohmann::json& j, MultibandEffect& effect) {
    effect = MultibandEffect{};
    effect.id = j.value("id", 1u);
    effect.bypassed = j.value("bypassed", false);
    effect.autoMakeup = j.value("autoMakeup", false);
    if (j.contains("bands")) j.at("bands").get_to(effect.bands);
    if (j.contains("crossovers")) j.at("crossovers").get_to(effect.crossovers);
}

void to_json(nlohmann::json& j, DeEsserMode mode) {
    switch (mode) {
        case DeEsserMode::Split: j = "Split"; break;
        case DeEsserMode::Wideband: j = "Wideband"; break;
    }
}

void from_json(const nlohmann::json& j, DeEsserMode& mode) {
    const auto name = j.get<std::string>();
    if (name == "Split") mode = DeEsserMode::Split;
    else if (name == "Wideband") mode = DeEsserMode::Wideband;
    else throw nlohmann::json::other_error::create(502, "unknown DeEsserMode '" + name + "'", &j);
}

void to_json(nlohmann::json& j, const DeEsserEffect& effect) {
    j = nlohmann::json{{"id", effect.id}, {"bypassed", effect.bypassed}, {"mode", effect.mode},
                       {"freqHz", effect.freqHz}, {"q", effect.q}, {"thresholdDb", effect.thresholdDb},
                       {"rangeDb", effect.rangeDb}, {"attackMs", effect.attackMs}, {"releaseMs", effect.releaseMs}};
}

void from_json(const nlohmann::json& j, DeEsserEffect& effect) {
    effect = DeEsserEffect{};
    effect.id = j.value("id", 1u);
    effect.bypassed = j.value("bypassed", false);
    if (j.contains("mode")) j.at("mode").get_to(effect.mode);
    effect.freqHz = j.value("freqHz", effect.freqHz);
    effect.q = j.value("q", effect.q);
    effect.thresholdDb = j.value("thresholdDb", effect.thresholdDb);
    effect.rangeDb = j.value("rangeDb", effect.rangeDb);
    effect.attackMs = j.value("attackMs", effect.attackMs);
    effect.releaseMs = j.value("releaseMs", effect.releaseMs);
}

void to_json(nlohmann::json& j, const LimiterEffect& effect) {
    j = nlohmann::json{{"id", effect.id}, {"bypassed", effect.bypassed},
                       {"inputGainDb", effect.inputGainDb}, {"ceilingDb", effect.ceilingDb},
                       {"lookaheadMs", effect.lookaheadMs}, {"releaseMs", effect.releaseMs}};
}

void from_json(const nlohmann::json& j, LimiterEffect& effect) {
    effect = LimiterEffect{};
    effect.id = j.value("id", 1u);
    effect.bypassed = j.value("bypassed", false);
    effect.inputGainDb = j.value("inputGainDb", effect.inputGainDb);
    effect.ceilingDb = j.value("ceilingDb", effect.ceilingDb);
    effect.lookaheadMs = j.value("lookaheadMs", effect.lookaheadMs);
    effect.releaseMs = j.value("releaseMs", effect.releaseMs);
}

void to_json(nlohmann::json& j, const FeedbackSuppressorEffect& effect) {
    j = nlohmann::json{{"id", effect.id}, {"bypassed", effect.bypassed},
                       {"strength", effect.strength},
                       {"attenuationDb", effect.attenuationDb},
                       {"gainReductionMultiplier", effect.gainReductionMultiplier},
                       {"attackMs", effect.attackMs},
                       {"holdMs", effect.holdMs},
                       {"releaseMs", effect.releaseMs},
                       {"detectionDelayMs", effect.detectionDelayMs},
                       {"inputSensitivityDb", effect.inputSensitivityDb},
                       {"detectorHighPassHz", effect.detectorHighPassHz},
                       {"detectorLowPassHz", effect.detectorLowPassHz}};
}

void from_json(const nlohmann::json& j, FeedbackSuppressorEffect& effect) {
    effect = FeedbackSuppressorEffect{};
    effect.id = j.value("id", 1u);
    effect.bypassed = j.value("bypassed", false);
    effect.strength = j.value("strength", effect.strength);
    effect.attenuationDb = j.value("attenuationDb", effect.attenuationDb);
    effect.gainReductionMultiplier = j.value("gainReductionMultiplier",
                                             effect.gainReductionMultiplier);
    effect.attackMs = j.value("attackMs", effect.attackMs);
    effect.holdMs = j.value("holdMs", effect.holdMs);
    effect.releaseMs = j.value("releaseMs", effect.releaseMs);
    effect.detectionDelayMs = j.value("detectionDelayMs", effect.detectionDelayMs);
    effect.inputSensitivityDb = j.value("inputSensitivityDb", effect.inputSensitivityDb);
    effect.detectorHighPassHz = j.value("detectorHighPassHz", effect.detectorHighPassHz);
    effect.detectorLowPassHz = j.value("detectorLowPassHz", effect.detectorLowPassHz);
}

void to_json(nlohmann::json& j, SpectralDenoiserMode mode) {
    switch (mode) {
        case SpectralDenoiserMode::Learn: j = "Learn"; break;
        case SpectralDenoiserMode::Adaptive: j = "Adaptive"; break;
    }
}

void from_json(const nlohmann::json& j, SpectralDenoiserMode& mode) {
    const auto name = j.get<std::string>();
    if (name == "Learn") mode = SpectralDenoiserMode::Learn;
    else if (name == "Adaptive") mode = SpectralDenoiserMode::Adaptive;
    else throw nlohmann::json::other_error::create(502, "unknown SpectralDenoiserMode '" + name + "'", &j);
}

void to_json(nlohmann::json& j, const SpectralDenoiserEffect& effect) {
    j = nlohmann::json{{"id", effect.id}, {"bypassed", effect.bypassed}, {"mode", effect.mode},
                       {"reductionDb", effect.reductionDb},
                       {"profileSampleRate", effect.profileSampleRate},
                       {"noiseProfile", effect.noiseProfile}};
    if (!effect.profileTrimDb.empty()) {
        j["profileTrimDb"] = effect.profileTrimDb;
    }
    if (effect.thresholdOffsetDb != 0.0f) {
        j["thresholdOffsetDb"] = effect.thresholdOffsetDb;
    }
    if (effect.mixPercent != kSpectralDenoiserMaxMixPercent) {
        j["mixPercent"] = effect.mixPercent;
    }
    if (effect.speed != 1.0f) {
        j["speed"] = effect.speed;
    }
    if (effect.learnSeconds != 1.0f) {
        j["learnSeconds"] = effect.learnSeconds;
    }
    if (effect.stereoLinkPercent != kSpectralDenoiserMaxStereoLinkPercent) {
        j["stereoLinkPercent"] = effect.stereoLinkPercent;
    }
}

void from_json(const nlohmann::json& j, SpectralDenoiserEffect& effect) {
    effect = SpectralDenoiserEffect{};
    effect.id = j.value("id", 1u);
    effect.bypassed = j.value("bypassed", false);
    if (j.contains("mode")) j.at("mode").get_to(effect.mode);
    effect.reductionDb = j.value("reductionDb", effect.reductionDb);
    effect.profileSampleRate = j.value("profileSampleRate", 0.0);
    if (j.contains("noiseProfile")) j.at("noiseProfile").get_to(effect.noiseProfile);
    if (j.contains("profileTrimDb")) j.at("profileTrimDb").get_to(effect.profileTrimDb);
    effect.thresholdOffsetDb = j.value("thresholdOffsetDb", 0.0f);
    effect.mixPercent = j.value("mixPercent", effect.mixPercent);
    effect.speed = j.value("speed", effect.speed);
    effect.learnSeconds = j.value("learnSeconds", effect.learnSeconds);
    effect.stereoLinkPercent = j.value("stereoLinkPercent", effect.stereoLinkPercent);
    // Listen is monitor-only and deliberately never restored from a scene.
    effect.listen = false;
}

void to_json(nlohmann::json& j, DynamicsEffectType type) {
    switch (type) {
        case DynamicsEffectType::Compressor: j = "Compressor"; break;
        case DynamicsEffectType::Gate: j = "Gate"; break;
    }
}

void from_json(const nlohmann::json& j, DynamicsEffectType& type) {
    const auto name = j.get<std::string>();
    if (name == "Compressor") type = DynamicsEffectType::Compressor;
    else if (name == "Gate") type = DynamicsEffectType::Gate;
    else throw nlohmann::json::other_error::create(502, "unknown DynamicsEffectType '" + name + "'", &j);
}

void to_json(nlohmann::json& j, const DynamicsEffect& effect) {
    j = nlohmann::json{{"id", effect.id}, {"type", effect.type}, {"bypassed", effect.bypassed},
                       {"thresholdDb", effect.thresholdDb}, {"ratio", effect.ratio},
                       {"attackMs", effect.attackMs}, {"releaseMs", effect.releaseMs},
                       {"makeupDb", effect.makeupDb}, {"inputGainDb", effect.inputGainDb},
                       {"kneeDb", effect.kneeDb}};
}

void from_json(const nlohmann::json& j, DynamicsEffect& effect) {
    effect = DynamicsEffect{};
    j.at("id").get_to(effect.id);
    j.at("type").get_to(effect.type);
    effect.bypassed = j.value("bypassed", false);
    effect.thresholdDb = j.value("thresholdDb", effect.thresholdDb);
    effect.ratio = j.value("ratio", effect.ratio);
    effect.attackMs = j.value("attackMs", effect.attackMs);
    effect.releaseMs = j.value("releaseMs", effect.releaseMs);
    effect.makeupDb = j.value("makeupDb", effect.makeupDb);
    effect.inputGainDb = j.value("inputGainDb", effect.inputGainDb);
    effect.kneeDb = j.value("kneeDb", effect.kneeDb);
}

void to_json(nlohmann::json& j, const InsertSlot& slot) {
    j = nlohmann::json{
        {"id", slot.id},
        {"pluginPath", slot.pluginPath},
        {"pluginName", slot.pluginName},
        {"bypassed", slot.bypassed},
    };
    // Optional: older v3 files have no plugin state and stay compact when a
    // slot was never saved with one.
    if (!slot.pluginState.empty()) {
        j["pluginState"] = slot.pluginState;
    }
    // Optional: absent in scenes written before the class identity was kept,
    // and 0 means "the bundle's first class", which is the old behaviour.
    if (slot.pluginUniqueId != 0) {
        j["pluginUniqueId"] = slot.pluginUniqueId;
    }
    // Optional: absent unless the insert uses a side chain, so scenes without
    // one stay byte-for-byte compatible with pre-v8 saves.
    if (slot.sideChainSource != SideChainSource::None) {
        j["sideChainSource"] = slot.sideChainSource;
        j["sideChainSourceId"] = slot.sideChainSourceId;
        if (slot.sideChainTap != SideChainTap::PostFader) {
            j["sideChainTap"] = slot.sideChainTap;
        }
        if (slot.sideChainGainDb != 0.0f) {
            j["sideChainGainDb"] = slot.sideChainGainDb;
        }
    }
}

void from_json(const nlohmann::json& j, InsertSlot& slot) {
    slot = InsertSlot{};
    j.at("id").get_to(slot.id);
    slot.pluginPath = j.value("pluginPath", std::string{});
    slot.pluginName = j.value("pluginName", std::string{});
    slot.pluginUniqueId = j.value("pluginUniqueId", 0);
    slot.bypassed = j.value("bypassed", false);
    slot.pluginState = j.value("pluginState", std::string{});
    // Absent keys keep the pre-v8 behaviour (no side chain).
    if (j.contains("sideChainSource")) {
        j.at("sideChainSource").get_to(slot.sideChainSource);
    }
    slot.sideChainSourceId = j.value("sideChainSourceId", 0u);
    if (j.contains("sideChainTap")) {
        j.at("sideChainTap").get_to(slot.sideChainTap);
    }
    slot.sideChainGainDb = j.value("sideChainGainDb", 0.0f);
}

void to_json(nlohmann::json& j, ChainItemType type) {
    switch (type) {
        case ChainItemType::Eq: j = "Eq"; break;
        case ChainItemType::Dynamics: j = "Dynamics"; break;
        case ChainItemType::Insert: j = "Insert"; break;
        case ChainItemType::Multiband: j = "Multiband"; break;
        case ChainItemType::DeEsser: j = "DeEsser"; break;
        case ChainItemType::Limiter: j = "Limiter"; break;
        case ChainItemType::FeedbackSuppressor: j = "FeedbackSuppressor"; break;
        case ChainItemType::SpectralDenoiser: j = "SpectralDenoiser"; break;
    }
}

void from_json(const nlohmann::json& j, ChainItemType& type) {
    const auto name = j.get<std::string>();
    if (name == "Eq") type = ChainItemType::Eq;
    else if (name == "Dynamics") type = ChainItemType::Dynamics;
    else if (name == "Insert") type = ChainItemType::Insert;
    else if (name == "Multiband") type = ChainItemType::Multiband;
    else if (name == "DeEsser") type = ChainItemType::DeEsser;
    else if (name == "Limiter") type = ChainItemType::Limiter;
    else if (name == "FeedbackSuppressor") type = ChainItemType::FeedbackSuppressor;
    else if (name == "SpectralDenoiser") type = ChainItemType::SpectralDenoiser;
    else throw nlohmann::json::other_error::create(503, "unknown ChainItemType '" + name + "'", &j);
}

void to_json(nlohmann::json& j, RackPreviewMode mode) {
    switch (mode) {
        case RackPreviewMode::Selected: j = "Selected"; break;
        case RackPreviewMode::Rotate: j = "Rotate"; break;
        case RackPreviewMode::AllLive: j = "AllLive"; break;
    }
}

void from_json(const nlohmann::json& j, RackPreviewMode& mode) {
    const auto name = j.get<std::string>();
    if (name == "Selected") mode = RackPreviewMode::Selected;
    else if (name == "Rotate") mode = RackPreviewMode::Rotate;
    else if (name == "AllLive") mode = RackPreviewMode::AllLive;
    else throw nlohmann::json::other_error::create(503, "unknown RackPreviewMode '" + name + "'", &j);
}

void to_json(nlohmann::json& j, const ChainItem& item) {
    j = nlohmann::json{{"type", item.type}, {"id", item.id}};
}

void from_json(const nlohmann::json& j, ChainItem& item) {
    item = ChainItem{};
    j.at("type").get_to(item.type);
    j.at("id").get_to(item.id);
}

namespace {

// Scenes saved while the limiter was still a dynamics type carry entries this
// build no longer instantiates; the rest load as their concrete effect type.
template <typename Effect>
std::vector<Effect> readEffects(const nlohmann::json& entries) {
    std::vector<Effect> effects;
    for (const auto& entry : entries) {
        if (entry.value("type", std::string()) == "Limiter") continue;
        effects.push_back(entry.get<Effect>());
    }
    return effects;
}

} // namespace

void to_json(nlohmann::json& j, const Strip& strip) {
    j = nlohmann::json{
        {"id", strip.id},
        {"name", strip.name},
        {"layout", strip.layout},
        {"gainDb", strip.gainDb},
        {"muted", strip.muted},
        {"inserts", strip.inserts},
        {"effects", strip.effects},
        {"multibandEffects", strip.multibandEffects},
        {"deEsserEffects", strip.deEsserEffects},
        {"limiterEffects", strip.limiterEffects},
        {"feedbackSuppressorEffects", strip.feedbackSuppressorEffects},
        {"spectralDenoiserEffects", strip.spectralDenoiserEffects},
        {"multibandRtaOffsetDb", strip.multibandRtaOffsetDb},
        {"multibandRtaWeighting", strip.multibandRtaWeighting},
        {"deEsserRtaOffsetDb", strip.deEsserRtaOffsetDb},
        {"deEsserRtaWeighting", strip.deEsserRtaWeighting},
        {"feedbackSuppressorRtaOffsetDb", strip.feedbackSuppressorRtaOffsetDb},
        {"feedbackSuppressorRtaWeighting", strip.feedbackSuppressorRtaWeighting},
        {"chain", strip.chain},
        {"eqEnabled", strip.eqEnabled},
        {"eqBands", strip.eqBands},
        {"eqDelayMs", strip.eqDelayMs},
        {"eqPhaseFlip", strip.eqPhaseFlip},
        {"colourHue", strip.colourHue},
        {"eqRtaOffsetDb", strip.eqRtaOffsetDb},
        {"eqRtaWeighting", strip.eqRtaWeighting},
    };
}

void from_json(const nlohmann::json& j, Strip& strip) {
    j.at("id").get_to(strip.id);
    strip.name = j.value("name", std::string{});
    if (j.contains("layout")) {
        j.at("layout").get_to(strip.layout);
    }
    strip.gainDb = j.value("gainDb", 0.0f);
    strip.muted = j.value("muted", false);
    if (j.contains("inserts")) {
        j.at("inserts").get_to(strip.inserts);
    }
    if (j.contains("effects")) {
        strip.effects = readEffects<DynamicsEffect>(j.at("effects"));
    }
    // The chain used to be read only beside "effects". A scene (or a hand
    // edit) that carries a chain without the legacy dynamics list still has
    // to restore its routing of the other effects.
    if (j.contains("chain")) j.at("chain").get_to(strip.chain);
    // v1/v2 files have no EQ keys: the strip keeps eqEnabled == false with the
    // flat defaultEqBands(), and v3 files written before the delay knob have
    // no eqDelayMs (0 ms).
    if (j.contains("multibandEffects")) j.at("multibandEffects").get_to(strip.multibandEffects);
    if (j.contains("deEsserEffects")) j.at("deEsserEffects").get_to(strip.deEsserEffects);
    if (j.contains("limiterEffects")) j.at("limiterEffects").get_to(strip.limiterEffects);
    if (j.contains("feedbackSuppressorEffects")) j.at("feedbackSuppressorEffects").get_to(strip.feedbackSuppressorEffects);
    if (j.contains("spectralDenoiserEffects")) j.at("spectralDenoiserEffects").get_to(strip.spectralDenoiserEffects);
    strip.multibandRtaOffsetDb = j.value("multibandRtaOffsetDb", 0.0f);
    strip.multibandRtaWeighting = j.value("multibandRtaWeighting", 0);
    strip.deEsserRtaOffsetDb = j.value("deEsserRtaOffsetDb", 0.0f);
    strip.deEsserRtaWeighting = j.value("deEsserRtaWeighting", 0);
    strip.feedbackSuppressorRtaOffsetDb = j.value("feedbackSuppressorRtaOffsetDb", 0.0f);
    strip.feedbackSuppressorRtaWeighting = j.value("feedbackSuppressorRtaWeighting", 0);
    strip.eqEnabled = j.value("eqEnabled", false);
    strip.eqDelayMs = j.value("eqDelayMs", 0.0f);
    strip.eqPhaseFlip = j.value("eqPhaseFlip", false);
    strip.colourHue = j.value("colourHue", 0.0f);
    // Absent before the offset was saved: 0 is the default view.
    strip.eqRtaOffsetDb = j.value("eqRtaOffsetDb", 0.0f);
    strip.eqRtaWeighting = j.value("eqRtaWeighting", 0);
    if (j.contains("eqBands")) {
        j.at("eqBands").get_to(strip.eqBands);
    }
}

void to_json(nlohmann::json& j, const Channel& channel) {
    to_json(j, static_cast<const Strip&>(channel));
    j["sourceKind"] = channel.sourceKind;
    j["sourceDeviceId"] = channel.sourceDeviceId;
    j["inputChannels"] = channel.inputChannels;
    j["trimDb"] = channel.trimDb;
    j["pan"] = channel.pan;
    j["soloed"] = channel.soloed;
}

void from_json(const nlohmann::json& j, Channel& channel) {
    channel = Channel{};
    from_json(j, static_cast<Strip&>(channel));
    if (j.contains("sourceKind")) {
        j.at("sourceKind").get_to(channel.sourceKind);
    }
    channel.sourceDeviceId = j.value("sourceDeviceId", std::string{});
    if (j.contains("inputChannels")) {
        j.at("inputChannels").get_to(channel.inputChannels);
    } else if (channel.layout == ChannelLayout::Stereo) {
        channel.inputChannels = {0, 1};
    }
    channel.trimDb = j.value("trimDb", 0.0f);
    channel.pan = j.value("pan", 0.0f);
    channel.soloed = j.value("soloed", false);
}

void to_json(nlohmann::json& j, const Output& output) {
    to_json(j, static_cast<const Strip&>(output));
    j["kind"] = output.kind;
    j["deviceId"] = output.deviceId;
    j["flipped"] = output.flipped;
}

void from_json(const nlohmann::json& j, Output& output) {
    output = Output{};
    from_json(j, static_cast<Strip&>(output));
    if (j.contains("kind")) {
        j.at("kind").get_to(output.kind);
    }
    output.deviceId = j.value("deviceId", std::string{});
    // Scenes written before the stream-output class was retired still carry
    // "streamOutput"; unknown keys are ignored, so they load as monitors.
    output.flipped = j.value("flipped", false);
}

void to_json(nlohmann::json& j, const Bus& bus) {
    to_json(j, static_cast<const Strip&>(bus));
    j["trimDb"] = bus.trimDb;
}

void from_json(const nlohmann::json& j, Bus& bus) {
    bus = Bus{};
    from_json(j, static_cast<Strip&>(bus));
    bus.trimDb = j.value("trimDb", 0.0f);
}

void to_json(nlohmann::json& j, const Tap& tap) {
    switch (tap.kind) {
        case Tap::Kind::Input: j = "Input"; break;
        case Tap::Kind::Item: j = nlohmann::json{{"item", tap.itemId}}; break;
        case Tap::Kind::Output: j = "Output"; break;
    }
}

void from_json(const nlohmann::json& j, Tap& tap) {
    tap = Tap{};
    if (j.is_string()) {
        const auto name = j.get<std::string>();
        if (name == "Input") tap.kind = Tap::Kind::Input;
        else if (name == "Output") tap.kind = Tap::Kind::Output;
        else throw nlohmann::json::other_error::create(502, "unknown Tap '" + name + "'", &j);
    } else if (j.is_object()) {
        tap.kind = Tap::Kind::Item;
        tap.itemId = j.value("item", 0u);
    }
}

void to_json(nlohmann::json& j, const Route& route) {
    j = nlohmann::json{
        {"channelId", route.channelId},
        {"outputId", route.outputId},
        {"sendGainDb", route.sendGainDb},
    };
    // Optional: only a split L/R send needs the extra key, so matched sends
    // keep the compact historical form.
    if (route.sendGainDbR.has_value()) {
        j["sendGainDbR"] = *route.sendGainDbR;
    }
    if (route.tap.kind != Tap::Kind::Output) {
        j["tap"] = route.tap;
    }
}

void from_json(const nlohmann::json& j, Route& route) {
    route = Route{};
    j.at("channelId").get_to(route.channelId);
    j.at("outputId").get_to(route.outputId);
    route.sendGainDb = j.value("sendGainDb", 0.0f);
    if (j.contains("sendGainDbR")) {
        route.sendGainDbR = j.value("sendGainDbR", route.sendGainDb);
    }
    if (j.contains("tap")) {
        j.at("tap").get_to(route.tap);
    }
}

void to_json(nlohmann::json& j, const BusSend& send) {
    j = nlohmann::json{
        {"channelId", send.channelId},
        {"busId", send.busId},
        {"sendGainDb", send.sendGainDb},
    };
    if (send.sendGainDbR.has_value()) {
        j["sendGainDbR"] = *send.sendGainDbR;
    }
    if (send.tap.kind != Tap::Kind::Output) {
        j["tap"] = send.tap;
    }
}

void from_json(const nlohmann::json& j, BusSend& send) {
    send = BusSend{};
    j.at("channelId").get_to(send.channelId);
    j.at("busId").get_to(send.busId);
    send.sendGainDb = j.value("sendGainDb", 0.0f);
    if (j.contains("sendGainDbR")) {
        send.sendGainDbR = j.value("sendGainDbR", send.sendGainDb);
    }
    if (j.contains("tap")) {
        j.at("tap").get_to(send.tap);
    }
}

void to_json(nlohmann::json& j, const BusRoute& route) {
    j = nlohmann::json{
        {"busId", route.busId},
        {"outputId", route.outputId},
        {"sendGainDb", route.sendGainDb},
    };
    if (route.sendGainDbR.has_value()) {
        j["sendGainDbR"] = *route.sendGainDbR;
    }
}

void from_json(const nlohmann::json& j, BusRoute& route) {
    route = BusRoute{};
    j.at("busId").get_to(route.busId);
    j.at("outputId").get_to(route.outputId);
    route.sendGainDb = j.value("sendGainDb", 0.0f);
    if (j.contains("sendGainDbR")) {
        route.sendGainDbR = j.value("sendGainDbR", route.sendGainDb);
    }
}

void to_json(nlohmann::json& j, const WatchedDevice& device) {
    j = nlohmann::json{
        {"isInput", device.isInput},
        {"stripId", device.stripId},
    };
}

void from_json(const nlohmann::json& j, WatchedDevice& device) {
    device = WatchedDevice{};
    device.isInput = j.value("isInput", false);
    device.stripId = j.value("stripId", 0u);
}

void to_json(nlohmann::json& j, const AudioSettings& settings) {
    j = nlohmann::json{
        {"inputBuffer", settings.inputBuffer},
        {"outputBuffer", settings.outputBuffer},
        {"latencyTuningOptIn", settings.latencyTuningOptIn},
        {"lowLatencyInput", settings.lowLatencyInput},
        {"preferExclusive", settings.preferExclusive},
        {"lowLatencyOutputs", settings.lowLatencyOutputs},
        {"minimalClockFifo", settings.minimalClockFifo},
        {"fullDuplexInput", settings.fullDuplexInput},
        {"autoReloadDevices", settings.autoReloadDevices},
        {"watchedDevices", settings.watchedDevices},
        {"rackThumbnailFps", settings.rackThumbnailFps},
        {"rackPreviewMode", settings.rackPreviewMode},
        {"slicedRender", settings.slicedRender},
        {"nonFiniteScrub", settings.nonFiniteScrub},
        {"healthReload", settings.healthReload},
        {"ladderRecovery", settings.ladderRecovery},
        {"wasapiProbeCapEnabled", settings.wasapiProbeCapEnabled},
        {"wasapiProbeCapChannels", settings.wasapiProbeCapChannels},
        {"healthYellowUnderrunsPerMinute", settings.healthYellowUnderrunsPerMinute},
        {"healthRedUnderrunsPerMinute", settings.healthRedUnderrunsPerMinute},
        {"healthPinnedToEdgeMs", settings.healthPinnedToEdgeMs},
    };
}

void from_json(const nlohmann::json& j, AudioSettings& settings) {
    settings = AudioSettings{};
    settings.inputBuffer = j.value("inputBuffer", 0);
    settings.outputBuffer = j.value("outputBuffer", 0);
    settings.latencyTuningOptIn = j.value("latencyTuningOptIn", false);
    settings.lowLatencyInput = j.value("lowLatencyInput", false);
    settings.preferExclusive = j.value("preferExclusive", false);
    settings.lowLatencyOutputs = j.value("lowLatencyOutputs", false);
    settings.minimalClockFifo = j.value("minimalClockFifo", false);
    settings.fullDuplexInput = j.value("fullDuplexInput", false);
    settings.autoReloadDevices = j.value("autoReloadDevices", false);
    settings.slicedRender = j.value("slicedRender", false);
    settings.nonFiniteScrub = j.value("nonFiniteScrub", false);
    settings.healthReload = j.value("healthReload", false);
    settings.ladderRecovery = j.value("ladderRecovery", false);
    settings.wasapiProbeCapEnabled = j.value("wasapiProbeCapEnabled", false);
    // Clamp defensively: the validator reports an out-of-range value, but a
    // hand-edited file must not reach the backend outside [2, 64].
    settings.wasapiProbeCapChannels =
        std::clamp(j.value("wasapiProbeCapChannels", 8), 2, 64);
    settings.healthYellowUnderrunsPerMinute =
        j.value("healthYellowUnderrunsPerMinute", 1.0);
    settings.healthRedUnderrunsPerMinute =
        j.value("healthRedUnderrunsPerMinute", 10.0);
    settings.healthPinnedToEdgeMs = j.value("healthPinnedToEdgeMs", 5000.0);
    // The version-aware opt-in gate lives in fromJson(Scene): before v5 the
    // individual keys could appear without the marker, and honouring them
    // would silently switch an old session to aggressive latency modes.
    // Absent in scenes written before rack thumbnails existed: the default
    // rate is what those sessions were already running at.
    settings.rackThumbnailFps = j.value("rackThumbnailFps", 15);
    if (j.contains("rackPreviewMode")) {
        j.at("rackPreviewMode").get_to(settings.rackPreviewMode);
    }
    if (j.contains("watchedDevices")) {
        j.at("watchedDevices").get_to(settings.watchedDevices);
    }
}

std::string toJson(const Scene& scene) {
    nlohmann::json j = {
        {"schemaVersion", kSchemaVersion},
        {"name", scene.name},
        {"sampleRate", scene.sampleRate},
        {"bufferFrames", scene.bufferFrames},
        {"channels", scene.channels},
        {"outputs", scene.outputs},
        {"routes", scene.routes},
    };
    if (!scene.buses.empty()) j["buses"] = scene.buses;
    if (!scene.busSends.empty()) j["busSends"] = scene.busSends;
    if (!scene.busRoutes.empty()) j["busRoutes"] = scene.busRoutes;
    if (scene.audio.has_value()) j["audio"] = *scene.audio;
    return j.dump();
}

SceneLoadResult fromJson(std::string_view text) {
    SceneLoadResult result;
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        const int schemaVersion = j.value("schemaVersion", 0);
        if (schemaVersion < 1 || schemaVersion > 11) {
            result.errors.push_back("unsupported schemaVersion " + std::to_string(schemaVersion) +
                                    ", expected 1 to 11");
            return result;
        }
        Scene scene;
        scene.schemaVersion = schemaVersion;
        scene.name = j.value("name", std::string{});
        scene.sampleRate = j.value("sampleRate", 48000.0);
        scene.bufferFrames = j.value("bufferFrames", 256);
        if (j.contains("channels")) {
            j.at("channels").get_to(scene.channels);
        }
        if (j.contains("outputs")) {
            j.at("outputs").get_to(scene.outputs);
        }
        if (j.contains("routes")) {
            j.at("routes").get_to(scene.routes);
        }
        if (schemaVersion >= 9) {
            if (j.contains("buses")) {
                j.at("buses").get_to(scene.buses);
            }
            if (j.contains("busSends")) {
                j.at("busSends").get_to(scene.busSends);
            }
            if (j.contains("busRoutes")) {
                j.at("busRoutes").get_to(scene.busRoutes);
            }
        } else if (schemaVersion == 8 && j.contains("auxBuses")) {
            std::vector<AuxBus> legacy;
            j.at("auxBuses").get_to(legacy);
            for (const AuxBus& old : legacy) {
                Bus bus;
                bus.id = old.id;
                bus.name = old.name;
                bus.gainDb = old.gainDb;
                scene.buses.push_back(std::move(bus));
                for (const std::uint32_t channelId : old.sourceChannelIds) {
                    BusSend send;
                    send.channelId = channelId;
                    send.busId = old.id;
                    scene.busSends.push_back(send);
                }
            }
            // A v8 bus key always used the post-gain signal, whatever tap the
            // file named; pinning PostFader keeps the same key level.
            const auto pinBusTap = [](Strip& strip) {
                for (InsertSlot& slot : strip.inserts) {
                    if (slot.sideChainSource == SideChainSource::Bus) {
                        slot.sideChainTap = SideChainTap::PostFader;
                    }
                }
            };
            for (Channel& channel : scene.channels) {
                pinBusTap(channel);
            }
            for (Output& output : scene.outputs) {
                pinBusTap(output);
            }
        }
        // Pre-v8 files never carry side-chain routing, even if a hand-edited
        // file contains the keys.
        if (schemaVersion < 8) {
            for (auto& channel : scene.channels) {
                for (auto& slot : channel.inserts) {
                    slot.sideChainSource = SideChainSource::None;
                    slot.sideChainSourceId = 0;
                    slot.sideChainTap = SideChainTap::PostFader;
                    slot.sideChainGainDb = 0.0f;
                }
            }
            for (auto& output : scene.outputs) {
                for (auto& slot : output.inserts) {
                    slot.sideChainSource = SideChainSource::None;
                    slot.sideChainSourceId = 0;
                    slot.sideChainTap = SideChainTap::PostFader;
                    slot.sideChainGainDb = 0.0f;
                }
            }
        }
        if (j.contains("audio")) {
            pluginrack::AudioSettings settings;
            j.at("audio").get_to(settings);
            // Before v5 the individual tuning keys could exist without the
            // explicit marker; honouring those would silently opt an old
            // session into aggressive latency modes. v5 and later wrote the
            // marker and the values together, so they are authoritative.
            if (schemaVersion < 5 && !settings.latencyTuningOptIn) {
                settings.lowLatencyInput = false;
                settings.preferExclusive = false;
                settings.lowLatencyOutputs = false;
                settings.minimalClockFifo = false;
                settings.fullDuplexInput = false;
            }
            // v6 fields are opt-in: a pre-v6 save never turns them on, even if
            // a hand-edited file happens to contain the keys.
            if (schemaVersion < 6) {
                settings.slicedRender = false;
                settings.nonFiniteScrub = false;
                settings.healthReload = false;
                settings.ladderRecovery = false;
            }
            // v7 fields are opt-in: a pre-v7 save never turns the probe cap on.
            if (schemaVersion < 7) {
                settings.wasapiProbeCapEnabled = false;
                settings.wasapiProbeCapChannels = 8;
            }
            scene.audio = std::move(settings);
        }
        result.scene = scene;
    } catch (const std::exception& e) {
        result.errors.push_back(e.what());
        result.scene.reset();
    }
    return result;
}

} // namespace pluginrack
