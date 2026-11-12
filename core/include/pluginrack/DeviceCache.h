#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pluginrack {

// What one endpoint cost to open and what it negotiated. The cache is
// advisory: it lets the second launch skip a slow channel-count x format
// probe, and a failed open from cache always falls through to the live probe.
struct EndpointCapabilities {
    int inputChannels = 0;
    int outputChannels = 0;
    std::vector<int> sampleRates;
    int negotiatedRate = 0;
    int negotiatedBuffer = 0;
    std::string mode = "shared";
    int probeMs = 0;
    std::string lastGood;                // ISO-8601, for diagnostics only
    std::int64_t lastGoodUnixMs = 0;     // staleness source of truth
};

class DeviceCache {
public:
    static constexpr int kSchemaVersion = 1;
    static constexpr std::int64_t kDefaultMaxAgeMs =
        30LL * 24LL * 60LL * 60LL * 1000LL; // 30 days

    bool empty() const noexcept { return endpoints_.empty(); }
    std::size_t size() const noexcept { return endpoints_.size(); }
    void clear() noexcept { endpoints_.clear(); }

    void put(const std::string& endpoint, const EndpointCapabilities& capabilities) {
        endpoints_[endpoint] = capabilities;
    }

    const EndpointCapabilities* find(const std::string& endpoint) const noexcept {
        const auto it = endpoints_.find(endpoint);
        return it == endpoints_.end() ? nullptr : &it->second;
    }

    static bool isStale(const EndpointCapabilities& capabilities,
                        std::int64_t nowUnixMs,
                        std::int64_t maxAgeMs = kDefaultMaxAgeMs) noexcept {
        if (capabilities.lastGoodUnixMs <= 0) return true;
        if (nowUnixMs <= capabilities.lastGoodUnixMs) return false;
        return nowUnixMs - capabilities.lastGoodUnixMs > maxAgeMs;
    }

    // Fresh entries only. A stale entry is deliberately not returned so the
    // opener falls through to today's full probe instead of wedging a device
    // on capabilities that no longer describe it.
    std::optional<EndpointCapabilities> lookupUsable(
        const std::string& endpoint, std::int64_t nowUnixMs,
        std::int64_t maxAgeMs = kDefaultMaxAgeMs) const {
        const EndpointCapabilities* found = find(endpoint);
        if (found == nullptr || isStale(*found, nowUnixMs, maxAgeMs)) {
            return std::nullopt;
        }
        return *found;
    }

    std::string toJson() const;

    // Returns nullopt on malformed JSON or an unknown schema. A bad cache is
    // never allowed to break startup: the caller simply runs the live probe.
    static std::optional<DeviceCache> fromJson(
        std::string_view text, std::vector<std::string>* errors = nullptr);

    bool loadFromFile(const std::string& path,
                      std::vector<std::string>* errors = nullptr);
    bool saveToFile(const std::string& path,
                    std::vector<std::string>* errors = nullptr) const;

private:
    std::map<std::string, EndpointCapabilities> endpoints_;
};

// Decide which capabilities the opener should use. A successful live probe
// always wins and is what the caller persists. Otherwise a fresh cached entry
// is advisory truth; a stale/missing entry means the caller must probe.
std::optional<EndpointCapabilities> resolveCapabilities(
    const DeviceCache& cache, const std::string& endpoint, bool liveProbeOk,
    const EndpointCapabilities& live, std::int64_t nowUnixMs,
    std::int64_t maxAgeMs = DeviceCache::kDefaultMaxAgeMs);

} // namespace pluginrack
