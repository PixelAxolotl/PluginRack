#include "pluginrack/SceneCodec.h"

#include <nlohmann/json.hpp>

namespace pluginrack {

constexpr int kSchemaVersion = 2;

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

void to_json(nlohmann::json& j, const InsertSlot& slot) {
    j = nlohmann::json{
        {"id", slot.id},
        {"pluginPath", slot.pluginPath},
        {"pluginName", slot.pluginName},
        {"bypassed", slot.bypassed},
    };
}

void from_json(const nlohmann::json& j, InsertSlot& slot) {
    slot = InsertSlot{};
    j.at("id").get_to(slot.id);
    slot.pluginPath = j.value("pluginPath", std::string{});
    slot.pluginName = j.value("pluginName", std::string{});
    slot.bypassed = j.value("bypassed", false);
}

void to_json(nlohmann::json& j, const Channel& channel) {
    j = nlohmann::json{
        {"id", channel.id},
        {"name", channel.name},
        {"sourceKind", channel.sourceKind},
        {"sourceDeviceId", channel.sourceDeviceId},
        {"layout", channel.layout},
        {"gainDb", channel.gainDb},
        {"pan", channel.pan},
        {"muted", channel.muted},
        {"soloed", channel.soloed},
        {"inserts", channel.inserts},
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
    channel.gainDb = j.value("gainDb", 0.0f);
    channel.pan = j.value("pan", 0.0f);
    channel.muted = j.value("muted", false);
    channel.soloed = j.value("soloed", false);
    if (j.contains("inserts")) {
        j.at("inserts").get_to(channel.inserts);
    }
}

void to_json(nlohmann::json& j, const Output& output) {
    j = nlohmann::json{
        {"id", output.id},
        {"name", output.name},
        {"kind", output.kind},
        {"deviceId", output.deviceId},
        {"inserts", output.inserts},
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
    if (j.contains("inserts")) {
        j.at("inserts").get_to(output.inserts);
    }
}

void to_json(nlohmann::json& j, const Route& route) {
    j = nlohmann::json{
        {"channelId", route.channelId},
        {"outputId", route.outputId},
        {"sendGainDb", route.sendGainDb},
    };
}

void from_json(const nlohmann::json& j, Route& route) {
    route = Route{};
    j.at("channelId").get_to(route.channelId);
    j.at("outputId").get_to(route.outputId);
    route.sendGainDb = j.value("sendGainDb", 0.0f);
}

std::string toJson(const Scene& scene) {
    const nlohmann::json j = {
        {"schemaVersion", kSchemaVersion},
        {"name", scene.name},
        {"sampleRate", scene.sampleRate},
        {"bufferFrames", scene.bufferFrames},
        {"channels", scene.channels},
        {"outputs", scene.outputs},
        {"routes", scene.routes},
    };
    return j.dump();
}

SceneLoadResult fromJson(std::string_view text) {
    SceneLoadResult result;
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        const int schemaVersion = j.value("schemaVersion", 0);
        if (schemaVersion != 1 && schemaVersion != 2) {
            result.errors.push_back("unsupported schemaVersion " + std::to_string(schemaVersion) +
                                    ", expected 1 or 2");
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
        result.scene = scene;
    } catch (const std::exception& e) {
        result.errors.push_back(e.what());
        result.scene.reset();
    }
    return result;
}

} // namespace pluginrack
