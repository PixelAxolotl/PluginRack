#include "pluginrack/SceneCodec.h"

#include <nlohmann/json.hpp>

namespace pluginrack {

constexpr int kSchemaVersion = 4;

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

void to_json(nlohmann::json& j, EqType type) {
    switch (type) {
        case EqType::HighPass: j = "HighPass"; break;
        case EqType::LowShelf: j = "LowShelf"; break;
        case EqType::Peak: j = "Peak"; break;
        case EqType::HighShelf: j = "HighShelf"; break;
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
}

void from_json(const nlohmann::json& j, InsertSlot& slot) {
    slot = InsertSlot{};
    j.at("id").get_to(slot.id);
    slot.pluginPath = j.value("pluginPath", std::string{});
    slot.pluginName = j.value("pluginName", std::string{});
    slot.bypassed = j.value("bypassed", false);
    slot.pluginState = j.value("pluginState", std::string{});
}

void to_json(nlohmann::json& j, ChainItemType type) {
    switch (type) {
        case ChainItemType::Eq: j = "Eq"; break;
        case ChainItemType::Dynamics: j = "Dynamics"; break;
        case ChainItemType::Insert: j = "Insert"; break;
    }
}

void from_json(const nlohmann::json& j, ChainItemType& type) {
    const auto name = j.get<std::string>();
    if (name == "Eq") type = ChainItemType::Eq;
    else if (name == "Dynamics") type = ChainItemType::Dynamics;
    else if (name == "Insert") type = ChainItemType::Insert;
    else throw nlohmann::json::other_error::create(503, "unknown ChainItemType '" + name + "'", &j);
}

void to_json(nlohmann::json& j, const ChainItem& item) {
    j = nlohmann::json{{"type", item.type}, {"id", item.id}};
}

void from_json(const nlohmann::json& j, ChainItem& item) {
    item = ChainItem{};
    j.at("type").get_to(item.type);
    j.at("id").get_to(item.id);
}

void to_json(nlohmann::json& j, const Channel& channel) {
    j = nlohmann::json{
        {"id", channel.id},
        {"name", channel.name},
        {"sourceKind", channel.sourceKind},
        {"sourceDeviceId", channel.sourceDeviceId},
        {"layout", channel.layout},
        {"inputChannels", channel.inputChannels},
        {"gainDb", channel.gainDb},
        {"trimDb", channel.trimDb},
        {"pan", channel.pan},
        {"muted", channel.muted},
        {"soloed", channel.soloed},
        {"inserts", channel.inserts},
        {"effects", channel.effects},
        {"chain", channel.chain},
        {"eqEnabled", channel.eqEnabled},
        {"eqBands", channel.eqBands},
        {"eqDelayMs", channel.eqDelayMs},
        {"eqRtaOffsetDb", channel.eqRtaOffsetDb},
        {"eqRtaWeighting", channel.eqRtaWeighting},
    };
}

void from_json(const nlohmann::json& j, Channel& channel) {
    channel = Channel{};
    j.at("id").get_to(channel.id);
    channel.name = j.value("name", std::string{});
    if (j.contains("sourceKind")) {
        j.at("sourceKind").get_to(channel.sourceKind);
    }
    channel.sourceDeviceId = j.value("sourceDeviceId", std::string{});
    if (j.contains("layout")) {
        j.at("layout").get_to(channel.layout);
    }
    if (j.contains("inputChannels")) {
        j.at("inputChannels").get_to(channel.inputChannels);
    } else {
        if (channel.layout == ChannelLayout::Stereo) {
            channel.inputChannels = {0, 1};
        } else {
            channel.inputChannels = {0};
        }
    }
    channel.gainDb = j.value("gainDb", 0.0f);
    channel.trimDb = j.value("trimDb", 0.0f);
    channel.pan = j.value("pan", 0.0f);
    channel.muted = j.value("muted", false);
    channel.soloed = j.value("soloed", false);
    if (j.contains("inserts")) {
        j.at("inserts").get_to(channel.inserts);
    }
    if (j.contains("effects")) {
        channel.effects.clear();
        for (const auto& entry : j.at("effects")) {
            // The limiter was retired; scenes saved with one still load.
            if (entry.value("type", std::string()) == "Limiter") continue;
            channel.effects.push_back(entry.get<DynamicsEffect>());
        }
        if (j.contains("chain")) j.at("chain").get_to(channel.chain);
    }
    // v1/v2 files have no EQ keys: the channel keeps eqEnabled == false with
    // the flat defaultSixBands() from Channel{} above, and v3 files written
    // before the delay knob have no eqDelayMs (0 ms).
    channel.eqEnabled = j.value("eqEnabled", false);
    channel.eqDelayMs = j.value("eqDelayMs", 0.0f);
    // Absent before the offset was saved: 0 is the default view.
    channel.eqRtaOffsetDb = j.value("eqRtaOffsetDb", 0.0f);
    channel.eqRtaWeighting = j.value("eqRtaWeighting", 0);
    if (j.contains("eqBands")) {
        j.at("eqBands").get_to(channel.eqBands);
    }
}

void to_json(nlohmann::json& j, const Output& output) {
    j = nlohmann::json{
        {"id", output.id},
        {"name", output.name},
        {"kind", output.kind},
        {"deviceId", output.deviceId},
        {"layout", output.layout},
        {"flipped", output.flipped},
        {"gainDb", output.gainDb},
        {"muted", output.muted},
        {"inserts", output.inserts},
        {"effects", output.effects},
        {"chain", output.chain},
        {"eqEnabled", output.eqEnabled},
        {"eqBands", output.eqBands},
        {"eqDelayMs", output.eqDelayMs},
        {"eqRtaOffsetDb", output.eqRtaOffsetDb},
        {"eqRtaWeighting", output.eqRtaWeighting},
    };
}

void from_json(const nlohmann::json& j, Output& output) {
    output = Output{};
    j.at("id").get_to(output.id);
    output.name = j.value("name", std::string{});
    if (j.contains("kind")) {
        j.at("kind").get_to(output.kind);
    }
    output.deviceId = j.value("deviceId", std::string{});
    if (j.contains("layout")) {
        j.at("layout").get_to(output.layout);
    }
    output.flipped = j.value("flipped", false);
    output.gainDb = j.value("gainDb", 0.0f);
    output.muted = j.value("muted", false);
    if (j.contains("inserts")) {
        j.at("inserts").get_to(output.inserts);
    }
    if (j.contains("effects")) {
        output.effects.clear();
        for (const auto& entry : j.at("effects")) {
            if (entry.value("type", std::string()) == "Limiter") continue;
            output.effects.push_back(entry.get<DynamicsEffect>());
        }
        if (j.contains("chain")) j.at("chain").get_to(output.chain);
    }
    // v1/v2 files have no EQ keys: the output keeps eqEnabled == false with
    // the flat defaultSixBands() from Output{} above, and v3 files written
    // before the delay knob have no eqDelayMs (0 ms).
    output.eqEnabled = j.value("eqEnabled", false);
    output.eqDelayMs = j.value("eqDelayMs", 0.0f);
    output.eqRtaOffsetDb = j.value("eqRtaOffsetDb", 0.0f);
    output.eqRtaWeighting = j.value("eqRtaWeighting", 0);
    if (j.contains("eqBands")) {
        j.at("eqBands").get_to(output.eqBands);
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
}

void from_json(const nlohmann::json& j, Route& route) {
    route = Route{};
    j.at("channelId").get_to(route.channelId);
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
        {"lowLatencyInput", settings.lowLatencyInput},
        {"preferExclusive", settings.preferExclusive},
        {"lowLatencyOutputs", settings.lowLatencyOutputs},
        {"minimalClockFifo", settings.minimalClockFifo},
        {"fullDuplexInput", settings.fullDuplexInput},
        {"autoReloadDevices", settings.autoReloadDevices},
        {"watchedDevices", settings.watchedDevices},
        {"rackThumbnailFps", settings.rackThumbnailFps},
    };
}

void from_json(const nlohmann::json& j, AudioSettings& settings) {
    settings = AudioSettings{};
    settings.inputBuffer = j.value("inputBuffer", 0);
    settings.outputBuffer = j.value("outputBuffer", 0);
    settings.lowLatencyInput = j.value("lowLatencyInput", false);
    settings.preferExclusive = j.value("preferExclusive", false);
    settings.lowLatencyOutputs = j.value("lowLatencyOutputs", false);
    settings.minimalClockFifo = j.value("minimalClockFifo", false);
    settings.fullDuplexInput = j.value("fullDuplexInput", false);
    settings.autoReloadDevices = j.value("autoReloadDevices", false);
    // Absent in scenes written before rack thumbnails existed: the default
    // rate is what those sessions were already running at.
    settings.rackThumbnailFps = j.value("rackThumbnailFps", 15);
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
    if (scene.audio.has_value()) j["audio"] = *scene.audio;
    return j.dump();
}

SceneLoadResult fromJson(std::string_view text) {
    SceneLoadResult result;
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        const int schemaVersion = j.value("schemaVersion", 0);
        if (schemaVersion < 1 || schemaVersion > 4) {
            result.errors.push_back("unsupported schemaVersion " + std::to_string(schemaVersion) +
                                    ", expected 1 to 4");
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
        if (j.contains("audio")) {
            pluginrack::AudioSettings settings;
            j.at("audio").get_to(settings);
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
