#include "pluginrack/DeviceCache.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace pluginrack {

namespace {

nlohmann::json capabilitiesToJson(const EndpointCapabilities& capabilities) {
    nlohmann::json j = {
        {"channels", {{"input", capabilities.inputChannels},
                      {"output", capabilities.outputChannels}}},
        {"sampleRates", capabilities.sampleRates},
        {"negotiated", {{"rate", capabilities.negotiatedRate},
                        {"buffer", capabilities.negotiatedBuffer},
                        {"mode", capabilities.mode}}},
        {"probeMs", capabilities.probeMs},
        {"lastGood", capabilities.lastGood},
        {"lastGoodUnixMs", capabilities.lastGoodUnixMs},
    };
    return j;
}

EndpointCapabilities capabilitiesFromJson(const nlohmann::json& j) {
    EndpointCapabilities capabilities;
    if (j.contains("channels")) {
        const auto& channels = j.at("channels");
        capabilities.inputChannels = channels.value("input", 0);
        capabilities.outputChannels = channels.value("output", 0);
    }
    if (j.contains("sampleRates")) {
        j.at("sampleRates").get_to(capabilities.sampleRates);
    }
    if (j.contains("negotiated")) {
        const auto& negotiated = j.at("negotiated");
        capabilities.negotiatedRate = negotiated.value("rate", 0);
        capabilities.negotiatedBuffer = negotiated.value("buffer", 0);
        capabilities.mode = negotiated.value("mode", std::string{"shared"});
    }
    capabilities.probeMs = j.value("probeMs", 0);
    capabilities.lastGood = j.value("lastGood", std::string{});
    capabilities.lastGoodUnixMs = j.value("lastGoodUnixMs", static_cast<std::int64_t>(0));
    return capabilities;
}

} // namespace

std::string DeviceCache::toJson() const {
    nlohmann::json endpoints = nlohmann::json::object();
    for (const auto& entry : endpoints_) {
        endpoints[entry.first] = capabilitiesToJson(entry.second);
    }
    const nlohmann::json root = {
        {"schema", kSchemaVersion},
        {"endpoints", std::move(endpoints)},
    };
    return root.dump();
}

std::optional<DeviceCache> DeviceCache::fromJson(
    std::string_view text, std::vector<std::string>* errors) {
    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        const int schema = root.value("schema", 0);
        if (schema != kSchemaVersion) {
            if (errors != nullptr) {
                errors->push_back("unsupported device-cache schema " +
                                  std::to_string(schema) + ", expected " +
                                  std::to_string(kSchemaVersion));
            }
            return std::nullopt;
        }

        DeviceCache cache;
        if (root.contains("endpoints")) {
            const auto& endpoints = root.at("endpoints");
            if (!endpoints.is_object()) {
                if (errors != nullptr) errors->push_back("device-cache endpoints is not an object");
                return std::nullopt;
            }
            for (auto it = endpoints.begin(); it != endpoints.end(); ++it) {
                cache.put(it.key(), capabilitiesFromJson(it.value()));
            }
        }
        return cache;
    } catch (const std::exception& e) {
        if (errors != nullptr) errors->push_back(e.what());
        return std::nullopt;
    }
}

bool DeviceCache::loadFromFile(const std::string& path,
                               std::vector<std::string>* errors) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (errors != nullptr) errors->push_back("cannot open " + path);
        return false;
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    auto loaded = fromJson(contents.str(), errors);
    if (!loaded.has_value()) return false;
    endpoints_ = std::move(loaded->endpoints_);
    return true;
}

bool DeviceCache::saveToFile(const std::string& path,
                             std::vector<std::string>* errors) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (errors != nullptr) errors->push_back("cannot write " + path);
        return false;
    }
    out << toJson();
    if (!out.good()) {
        if (errors != nullptr) errors->push_back("write failed for " + path);
        return false;
    }
    return true;
}

std::optional<EndpointCapabilities> resolveCapabilities(
    const DeviceCache& cache, const std::string& endpoint, bool liveProbeOk,
    const EndpointCapabilities& live, std::int64_t nowUnixMs,
    std::int64_t maxAgeMs) {
    if (liveProbeOk) {
        return live;
    }
    return cache.lookupUsable(endpoint, nowUnixMs, maxAgeMs);
}

} // namespace pluginrack
