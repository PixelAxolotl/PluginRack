#include "pluginrack/MixerEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "pluginrack/SampleScrub.h"
#include "pluginrack/SceneValidator.h"

namespace pluginrack {

namespace {

constexpr float kMinAudibleGainDb = -60.0f;
constexpr float kPi = 3.14159265358979323846f;

float dbToLinear(float db) {
    if (db <= kMinAudibleGainDb) {
        return 0.0f;
    }
    return std::pow(10.0f, db / 20.0f);
}

int clampFrames(int frames, int maxFrames) noexcept {
    if (frames < 0) {
        return 0;
    }
    if (frames > maxFrames) {
        return maxFrames;
    }
    return frames;
}

// NaN hygiene at the EQ tap: non-finite samples would pass straight through
// a biquad (the filter guards only its own state), so flush them to silence
// before the strip's EQ runs. Strips with EQ bypassed keep the historical
// pass-through behaviour untouched.
void flushNonFinite(float* left, float* right, int frames) noexcept {
    for (int i = 0; i < frames; ++i) {
        if (!std::isfinite(left[i])) {
            left[i] = 0.0f;
        }
        if (!std::isfinite(right[i])) {
            right[i] = 0.0f;
        }
    }
}

// Chain items name their effect by id; the processor vectors are built in
// scene order, so the id is also the position to run.
template <typename Effect>
std::optional<std::size_t> findByEffectId(const std::vector<Effect>& effects, std::uint32_t id) {
    for (std::size_t i = 0; i < effects.size(); ++i) {
        if (effects[i].id == id) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

MixerEngine::MixerEngine() : dsp_(std::make_unique<DspState>()) {}

void MixerEngine::setPluginFactory(IPluginFactory* factory) noexcept {
    pluginFactory_ = factory;
}

bool MixerEngine::prepare(const Scene& scene, int maxBlockFrames, std::string* error) {
    const std::vector<SceneError> errors = validate(scene);
    if (!errors.empty()) {
        if (error != nullptr) {
            *error = errors.front().message;
        }
        return false;
    }
    if (scene.outputs.empty()) {
        if (error != nullptr) {
            *error = "scene has no outputs";
        }
        return false;
    }

    std::unordered_map<std::uint32_t, std::size_t> channelIndex;
    channelIndex.reserve(scene.channels.size() * 2 + 1);
    std::unordered_map<std::uint32_t, std::size_t> outputIndex;
    outputIndex.reserve(scene.outputs.size() * 2 + 1);
    std::unordered_map<std::uint32_t, std::size_t> busIndex;
    busIndex.reserve(scene.buses.size() * 2 + 1);

    channels_.clear();
    channels_.reserve(scene.channels.size());
    bool anySolo = false;
    for (std::size_t i = 0; i < scene.channels.size(); ++i) {
        const Channel& channel = scene.channels[i];
        channelIndex.emplace(channel.id, i);
        const float theta = (channel.pan + 1.0f) * kPi / 4.0f;
        ChannelCache cache;
        cache.gain = dbToLinear(channel.gainDb + channel.trimDb);
        cache.panLeft = std::cos(theta);
        cache.panRight = std::sin(theta);
        cache.muted = channel.muted;
        cache.soloed = channel.soloed;
        channels_.push_back(cache);
        anySolo = anySolo || channel.soloed;
    }
    for (std::size_t i = 0; i < scene.outputs.size(); ++i) {
        outputIndex.emplace(scene.outputs[i].id, i);
    }
    outputGains_.clear();
    outputGains_.reserve(scene.outputs.size());
    outputMuted_.clear();
    outputMuted_.reserve(scene.outputs.size());
    for (const Output& output : scene.outputs) {
        outputGains_.push_back(dbToLinear(output.gainDb));
        outputMuted_.push_back(output.muted ? 1 : 0);
    }

    routes_.clear();
    routes_.reserve(scene.routes.size());
    for (const Route& route : scene.routes) {
        RouteCache cache;
        cache.channelIndex = channelIndex[route.channelId];
        cache.outputIndex = outputIndex[route.outputId];
        cache.sendGainL = dbToLinear(route.sendGainDb);
        cache.sendGainR = dbToLinear(route.sendGainDbR.value_or(route.sendGainDb));
        routes_.push_back(cache);
    }

    buses_.clear();
    buses_.reserve(scene.buses.size());
    for (std::size_t i = 0; i < scene.buses.size(); ++i) {
        const Bus& bus = scene.buses[i];
        busIndex.emplace(bus.id, i);
        BusCache cache;
        cache.gain = dbToLinear(bus.gainDb);
        cache.trim = dbToLinear(bus.trimDb);
        cache.muted = bus.muted;
        buses_.push_back(cache);
    }
    busSends_.clear();
    busSends_.reserve(scene.busSends.size());
    for (const BusSend& send : scene.busSends) {
        BusSendCache cache;
        cache.channelIndex = channelIndex[send.channelId];
        cache.busIndex = busIndex[send.busId];
        cache.sendGainL = dbToLinear(send.sendGainDb);
        cache.sendGainR = dbToLinear(send.sendGainDbR.value_or(send.sendGainDb));
        busSends_.push_back(cache);
    }
    busRoutes_.clear();
    busRoutes_.reserve(scene.busRoutes.size());
    for (const BusRoute& route : scene.busRoutes) {
        BusRouteCache cache;
        cache.busIndex = busIndex[route.busId];
        cache.outputIndex = outputIndex[route.outputId];
        cache.sendGainL = dbToLinear(route.sendGainDb);
        cache.sendGainR = dbToLinear(route.sendGainDbR.value_or(route.sendGainDb));
        busRoutes_.push_back(cache);
    }

    const int maxFrames = maxBlockFrames < 0 ? 0 : maxBlockFrames;
    maxBlockFrames_ = maxFrames;
    outputCount_ = scene.outputs.size();
    anySolo_ = anySolo;
    sampleRate_ = scene.sampleRate;

    const std::size_t stereoFrames = static_cast<std::size_t>(maxFrames) * 2;
    scratch_.assign(channels_.size() * stereoFrames, 0.0f);
    accumulators_.assign(outputCount_ * stereoFrames, 0.0f);
    busBuffers_.assign(buses_.size() * stereoFrames, 0.0f);

    insertChains_.clear();
    if (pluginFactory_ != nullptr) {
        insertChains_.reserve(scene.channels.size());
        for (const Channel& channel : scene.channels) {
            insertChains_.push_back(std::make_unique<InsertChain>(
                channel.inserts, *pluginFactory_, scene.sampleRate, maxFrames,
                scrubEnabled_));
        }
    }
    outputInsertChains_.clear();
    if (pluginFactory_ != nullptr) {
        outputInsertChains_.reserve(scene.outputs.size());
        for (const Output& output : scene.outputs) {
            outputInsertChains_.push_back(std::make_unique<InsertChain>(
                output.inserts, *pluginFactory_, scene.sampleRate, maxFrames,
                scrubEnabled_));
        }
    }
    busInsertChains_.clear();
    if (pluginFactory_ != nullptr) {
        busInsertChains_.reserve(scene.buses.size());
        for (const Bus& bus : scene.buses) {
            busInsertChains_.push_back(std::make_unique<InsertChain>(
                bus.inserts, *pluginFactory_, scene.sampleRate, maxFrames,
                scrubEnabled_));
        }
    }

    dsp_ = std::make_unique<DspState>();
    dsp_->channels.reserve(scene.channels.size());
    for (const Channel& channel : scene.channels) {
        dsp_->channels.push_back(buildStrip(channel, scene.sampleRate, maxFrames));
    }
    dsp_->outputs.reserve(scene.outputs.size());
    for (const Output& output : scene.outputs) {
        dsp_->outputs.push_back(buildStrip(output, scene.sampleRate, maxFrames));
    }
    dsp_->buses.reserve(scene.buses.size());
    for (const Bus& bus : scene.buses) {
        dsp_->buses.push_back(buildStrip(bus, scene.sampleRate, maxFrames));
    }

    stoppedChannels_.assign(channels_.size(), 0);
    disabledOutputs_.assign(outputCount_, 0);

    channelIds_.clear();
    channelIds_.reserve(scene.channels.size());
    for (const Channel& channel : scene.channels) {
        channelIds_.push_back(channel.id);
    }
    outputIds_.clear();
    outputIds_.reserve(scene.outputs.size());
    for (const Output& output : scene.outputs) {
        outputIds_.push_back(output.id);
    }
    busIds_.clear();
    busIds_.reserve(scene.buses.size());
    for (const Bus& bus : scene.buses) {
        busIds_.push_back(bus.id);
    }
    meters_.setTargets(channelIds_.data(), channelIds_.size(),
                       outputIds_.data(), outputIds_.size(),
                       busIds_.data(), busIds_.size());

    // Taps: capture the signal at the point each route/send asked for and
    // delay every path to the output by the widest chain latency, so a dry
    // route and a bus route that started from the same point line up again.
    const int ringCapacity = kMaxTapDelaySamples + maxFrames + 2;
    tapScratch_.assign(static_cast<std::size_t>(maxFrames) * 2, 0.0f);
    auto clampDelay = [](int delay) {
        return delay < 0 ? 0 : (delay > kMaxTapDelaySamples ? kMaxTapDelaySamples : delay);
    };
    // An Item tap is keyed by (type, id): built-in families mint ids
    // independently, so a Dynamics row and a Feedback Suppressor row can both
    // be id 1 and only the type tells their taps apart.
    const auto itemTapKey = [](ChainItemType type, std::uint32_t id) {
        return (static_cast<std::uint64_t>(type) << 32) | id;
    };
    auto computeStripLatency = [&itemTapKey](const Strip& strip, const InsertChain* chain,
                                             double sampleRate,
                                             std::unordered_map<std::uint64_t, int>& itemLatency) {
        // The EQ delay ring runs at an Eq chain row, at that row's position;
        // a strip without the row runs it as the last appended step (see
        // buildStrip) and bills it there. Both paths bill the same expression,
        // matching the live engine's chainLatencySamples.
        int total = 0;
        bool usedEq = false;
        for (std::size_t row = 0; row < strip.chain.size(); ++row) {
            const ChainItem& item = strip.chain[row];
            int added = 0;
            if (item.type == ChainItemType::Insert) {
                added = chain != nullptr ? chain->latencySamples(item.id) : 0;
            } else if (item.type == ChainItemType::Limiter) {
                if (const auto index = findByEffectId(strip.limiterEffects, item.id)) {
                    added = limiterLatencySamples(strip.limiterEffects[*index], sampleRate);
                }
            } else if (item.type == ChainItemType::SpectralDenoiser) {
                if (const auto index = findByEffectId(strip.spectralDenoiserEffects, item.id)) {
                    added = spectralDenoiserLatencySamples(
                        strip.spectralDenoiserEffects[*index]);
                }
            } else if (item.type == ChainItemType::Eq) {
                // Offline keeps only the bypass flag, so it stands in for the
                // live engine's separate eqBypassed argument.
                usedEq = true;
                added = eqLatencySamples(strip.eqEnabled, false, strip.eqDelayMs, sampleRate);
            }
            total += added;
            // Every row can be named by its own (id, type), so each one
            // publishes the running total at its position.
            itemLatency[itemTapKey(item.type, item.id)] = total;
        }
        // An effect with no chain row runs nothing, so it bills nothing. That
        // matches live render, where rows are the only thing that engages a
        // built-in.
        // The EQ's fallback step is appended last by buildStrip(), so it bills
        // last here. The sum is order-independent; the position documents the
        // run order for the next reader.
        if (!usedEq) {
            total += eqLatencySamples(strip.eqEnabled, false, strip.eqDelayMs, sampleRate);
        }
        return total;
    };
    auto addTap = [](std::vector<TapState>& taps, const Tap& tap) {
        for (const TapState& existing : taps) {
            if (existing.tap == tap) {
                return;
            }
        }
        TapState state;
        state.tap = tap;
        taps.push_back(std::move(state));
    };
    auto findTap = [](const std::vector<TapState>& taps, const Tap& tap) -> std::size_t {
        for (std::size_t i = 0; i < taps.size(); ++i) {
            if (taps[i].tap == tap) {
                return i;
            }
        }
        return 0;
    };

    channelTaps_.assign(scene.channels.size(), {});
    for (const Route& route : scene.routes) {
        const std::size_t c = static_cast<std::size_t>(channelIndex[route.channelId]);
        addTap(channelTaps_[c], pluginrack::resolveItemTap(scene.channels[c].chain, route.tap));
    }
    for (const BusSend& send : scene.busSends) {
        const std::size_t c = static_cast<std::size_t>(channelIndex[send.channelId]);
        addTap(channelTaps_[c], pluginrack::resolveItemTap(scene.channels[c].chain, send.tap));
    }
    std::vector<int> channelLatencies(scene.channels.size(), 0);
    for (std::size_t c = 0; c < scene.channels.size(); ++c) {
        std::unordered_map<std::uint64_t, int> itemLatency;
        channelLatencies[c] = computeStripLatency(
            scene.channels[c],
            c < insertChains_.size() ? insertChains_[c].get() : nullptr,
            scene.sampleRate,
            itemLatency);
        for (TapState& state : channelTaps_[c]) {
            if (state.tap.kind == Tap::Kind::Input) {
                state.latencySamples = 0;
            } else if (state.tap.kind == Tap::Kind::Output) {
                state.latencySamples = channelLatencies[c];
            } else {
                // TapState.tap is resolved: a legacy id-only tap was pinned to
                // the first row carrying the id when it was added above.
                const int row =
                    pluginrack::firstChainItemIndex(scene.channels[c].chain, state.tap);
                int latency = channelLatencies[c];
                if (row >= 0) {
                    const ChainItem& item = scene.channels[c].chain[static_cast<std::size_t>(row)];
                    const auto it = itemLatency.find(itemTapKey(item.type, item.id));
                    if (it != itemLatency.end()) latency = it->second;
                }
                state.latencySamples = latency;
            }
            state.ring.prepare(ringCapacity);
        }
    }

    busLatencies_.assign(scene.buses.size(), 0);
    for (std::size_t b = 0; b < scene.buses.size(); ++b) {
        std::unordered_map<std::uint64_t, int> itemLatency;
        busLatencies_[b] = computeStripLatency(
            scene.buses[b],
            b < busInsertChains_.size() ? busInsertChains_[b].get() : nullptr,
            scene.sampleRate,
            itemLatency);
    }
    int maxBusLatency = 0;
    for (const BusRoute& route : scene.busRoutes) {
        const int latency = busLatencies_[busIndex[route.busId]];
        if (latency > maxBusLatency) {
            maxBusLatency = latency;
        }
    }
    busCompensation_.clear();
    busCompensation_.resize(scene.buses.size());
    for (DelayRing& ring : busCompensation_) {
        ring.prepare(ringCapacity);
    }

    for (std::size_t i = 0; i < scene.routes.size(); ++i) {
        RouteCache& cache = routes_[i];
        const std::vector<TapState>& taps = channelTaps_[cache.channelIndex];
        cache.tapIndex = findTap(
            taps, pluginrack::resolveItemTap(
                      scene.channels[cache.channelIndex].chain, scene.routes[i].tap));
        const int tapLatency = taps[cache.tapIndex].latencySamples;
        cache.delaySamples = clampDelay(
            channelLatencies[cache.channelIndex] - tapLatency + maxBusLatency);
    }
    for (std::size_t i = 0; i < scene.busSends.size(); ++i) {
        BusSendCache& cache = busSends_[i];
        const std::vector<TapState>& taps = channelTaps_[cache.channelIndex];
        cache.tapIndex = findTap(
            taps, pluginrack::resolveItemTap(
                      scene.channels[cache.channelIndex].chain, scene.busSends[i].tap));
        const int tapLatency = taps[cache.tapIndex].latencySamples;
        cache.delaySamples = clampDelay(
            channelLatencies[cache.channelIndex] - tapLatency);
    }
    for (std::size_t i = 0; i < scene.busRoutes.size(); ++i) {
        BusRouteCache& cache = busRoutes_[i];
        cache.delaySamples = clampDelay(maxBusLatency - busLatencies_[cache.busIndex]);
    }
    return true;
}

bool MixerEngine::prepare(const Scene& scene, const SceneAvailability& availability,
                          int maxBlockFrames, std::string* error) {
    if (!prepare(scene, maxBlockFrames, error)) {
        return false;
    }
    for (const std::uint32_t index : availability.stoppedChannels) {
        if (index < stoppedChannels_.size()) {
            stoppedChannels_[index] = 1;
        }
    }
    for (const std::uint32_t index : availability.disabledOutputs) {
        if (index < disabledOutputs_.size()) {
            disabledOutputs_[index] = 1;
        }
    }
    return true;
}

MixerEngine::StripDsp MixerEngine::buildStrip(const Strip& source, double sampleRate, int maxFrames) {
    StripDsp strip;
    strip.eq.prepare(sampleRate);
    strip.eq.setBands(source.eqBands);
    strip.eq.setDelayMs(source.eqDelayMs);
    strip.eqEnabled = source.eqEnabled;

    strip.dynamics.reserve(source.effects.size());
    for (const DynamicsEffect& effect : source.effects) {
        DynamicsProcessor processor;
        processor.prepare(sampleRate);
        processor.setEffect(effect);
        strip.dynamics.push_back(std::move(processor));
    }
    strip.multiband.reserve(source.multibandEffects.size());
    for (const MultibandEffect& effect : source.multibandEffects) {
        MultibandProcessor processor;
        processor.prepare(sampleRate, maxFrames);
        processor.setEffect(effect);
        strip.multiband.push_back(std::move(processor));
    }
    strip.deEssers.reserve(source.deEsserEffects.size());
    for (const DeEsserEffect& effect : source.deEsserEffects) {
        DeEsserProcessor processor;
        processor.prepare(sampleRate);
        processor.setEffect(effect);
        strip.deEssers.push_back(std::move(processor));
    }
    strip.limiters.reserve(source.limiterEffects.size());
    for (const LimiterEffect& effect : source.limiterEffects) {
        LimiterProcessor processor;
        processor.prepare(sampleRate);
        processor.setEffect(effect);
        strip.limiters.push_back(std::move(processor));
    }
    strip.feedbackSuppressors.reserve(source.feedbackSuppressorEffects.size());
    for (const FeedbackSuppressorEffect& effect : source.feedbackSuppressorEffects) {
        FeedbackSuppressorProcessor processor;
        processor.prepare(sampleRate);
        processor.setEffect(effect);
        strip.feedbackSuppressors.push_back(std::move(processor));
    }
    strip.spectralDenoisers.reserve(source.spectralDenoiserEffects.size());
    for (const SpectralDenoiserEffect& effect : source.spectralDenoiserEffects) {
        SpectralDenoiserProcessor processor;
        processor.prepare(sampleRate);
        processor.setEffect(effect);
        strip.spectralDenoisers.push_back(std::move(processor));
    }

    // Resolve the running order once. The scene's chain names effects by id;
    // an effect the chain leaves out does not run at all, matching live render,
    // where a row is the only thing that engages a built-in. Inserts render
    // through their own InsertChain and are skipped here. The EQ keeps a row
    // like live render, because that row runs it in place; without a row it
    // runs as the last appended step, the same place the live engine's
    // fallback pass runs it.
    bool usedEq = false;
    for (std::size_t row = 0; row < source.chain.size(); ++row) {
        const ChainItem& item = source.chain[row];
        // An item tap names a row by (id, type); a duplicate row of the same
        // family must not push the ring. Same rule as
        // pluginrack::firstChainItemIndex.
        const bool tapOwned =
            pluginrack::firstChainItemIndex(source.chain, item) == static_cast<int>(row);
        auto addStep = [&strip, &item, tapOwned](std::size_t index) {
            strip.steps.push_back({item.type, index, item.id, tapOwned});
        };
        switch (item.type) {
        case ChainItemType::Dynamics:
            if (const auto index = findByEffectId(source.effects, item.id)) {
                addStep(*index);
            } else {
                // Stale row the validator tolerated: keep the slot so a tap
                // aimed at it still sees the signal at this chain position.
                addStep(static_cast<std::size_t>(-1));
            }
            break;
        case ChainItemType::Multiband:
            if (const auto index = findByEffectId(source.multibandEffects, item.id)) {
                addStep(*index);
            } else {
                addStep(static_cast<std::size_t>(-1));
            }
            break;
        case ChainItemType::DeEsser:
            if (const auto index = findByEffectId(source.deEsserEffects, item.id)) {
                addStep(*index);
            } else {
                addStep(static_cast<std::size_t>(-1));
            }
            break;
        case ChainItemType::Limiter:
            if (const auto index = findByEffectId(source.limiterEffects, item.id)) {
                addStep(*index);
            } else {
                addStep(static_cast<std::size_t>(-1));
            }
            break;
        case ChainItemType::FeedbackSuppressor:
            if (const auto index = findByEffectId(source.feedbackSuppressorEffects, item.id)) {
                addStep(*index);
            } else {
                addStep(static_cast<std::size_t>(-1));
            }
            break;
        case ChainItemType::SpectralDenoiser:
            if (const auto index = findByEffectId(source.spectralDenoiserEffects, item.id)) {
                addStep(*index);
            } else {
                addStep(static_cast<std::size_t>(-1));
            }
            break;
        case ChainItemType::Eq:
            // Like live render, the EQ runs at this row when one exists. The
            // sentinel index marks a row that owns no processor; the step loop
            // probes the strip's single EqEngine instead.
            usedEq = true;
            addStep(static_cast<std::size_t>(-1));
            break;
        case ChainItemType::Insert:
            // Live inserts are tapped by the InsertChain itself; a row whose
            // slot is gone still needs its position in the step order.
            if (!findByEffectId(source.inserts, item.id)) {
                addStep(static_cast<std::size_t>(-1));
            }
            break;
        }
    }
    // The EQ has no processor vector to scan, so its unchained fallback is a
    // single step closing the appended order, matching the live engine's
    // fallback pass. The sentinel index and the cleared tap flag
    // keep it from owning an item tap: with no row there is no id to name it.
    // The step is appended even while the strip is bypassed: the EQ can be
    // enabled after prepare (setChannelEqBypassed), and the live engine picks
    // that up because it re-reads eqBypassed every block. processStrip gates
    // the step on strip.eqEnabled and computeStripLatency bills
    // eqLatencySamples(strip.eqEnabled, ...), which is 0 while disabled, so a
    // dormant step neither processes nor delays anything.
    if (!usedEq) {
        strip.steps.push_back({ChainItemType::Eq, static_cast<std::size_t>(-1), 0, false});
    }
    return strip;
}

void MixerEngine::processStrip(StripDsp& strip, float* left, float* right, int frames,
                               void (*afterStep)(void*, ChainItemType, std::uint32_t),
                               void* ctx) {
    for (const Step& step : strip.steps) {
        switch (step.type) {
        case ChainItemType::Dynamics:
            if (step.index < strip.dynamics.size()) strip.dynamics[step.index].process(left, right, frames);
            break;
        case ChainItemType::Multiband:
            if (step.index < strip.multiband.size()) strip.multiband[step.index].process(left, right, frames);
            break;
        case ChainItemType::DeEsser:
            if (step.index < strip.deEssers.size()) strip.deEssers[step.index].process(left, right, frames);
            break;
        case ChainItemType::Limiter:
            if (step.index < strip.limiters.size()) strip.limiters[step.index].process(left, right, frames);
            break;
        case ChainItemType::FeedbackSuppressor:
            if (step.index < strip.feedbackSuppressors.size()) strip.feedbackSuppressors[step.index].process(left, right, frames);
            break;
        case ChainItemType::SpectralDenoiser:
            if (step.index < strip.spectralDenoisers.size()) strip.spectralDenoisers[step.index].process(left, right, frames);
            break;
        case ChainItemType::Eq:
            // Same order as live render: the EQ runs at the row, and its tap
            // below sees the post-EQ signal. A strip without the row reaches
            // this case through the fallback step buildStrip appends last.
            if (strip.eqEnabled) {
                flushNonFinite(left, right, frames);
                strip.eq.process(left, right, frames);
            }
            break;
        case ChainItemType::Insert:
            break;
        }
        if (afterStep != nullptr && step.tapOwned) {
            afterStep(ctx, step.type, step.id);
        }
    }
}

void MixerEngine::pushItemTap(void* ctx, ChainItemType type, std::uint32_t itemId) {
    auto* push = static_cast<TapPush*>(ctx);
    for (TapState& state : *push->taps) {
        if (state.tap.kind == Tap::Kind::Item && state.tap.itemId == itemId
            && (!state.tap.hasItemType || state.tap.itemType == type)) {
            state.ring.push(push->left, push->right, push->frames);
        }
    }
}

void MixerEngine::pushInsertTap(void* ctx, std::uint32_t itemId) {
    // InsertChain pushes per slot id; the tap must name an Insert row, not a
    // built-in row that happens to share the id.
    pushItemTap(ctx, ChainItemType::Insert, itemId);
}

void MixerEngine::setChannelEq(int ch, const std::vector<EqBand>& bands) {    if (dsp_ == nullptr) {
        return;
    }
    std::scoped_lock lock(dsp_->mutex);
    if (ch < 0 || static_cast<std::size_t>(ch) >= dsp_->channels.size()) {
        return;
    }
    dsp_->channels[static_cast<std::size_t>(ch)].eq.setBands(bands);
}

void MixerEngine::setOutputEq(int out, const std::vector<EqBand>& bands) {
    if (dsp_ == nullptr) {
        return;
    }
    std::scoped_lock lock(dsp_->mutex);
    if (out < 0 || static_cast<std::size_t>(out) >= dsp_->outputs.size()) {
        return;
    }
    dsp_->outputs[static_cast<std::size_t>(out)].eq.setBands(bands);
}

void MixerEngine::setChannelEqBypassed(int ch, bool bypassed) {
    if (dsp_ == nullptr) {
        return;
    }
    std::scoped_lock lock(dsp_->mutex);
    if (ch < 0 || static_cast<std::size_t>(ch) >= dsp_->channels.size()) {
        return;
    }
    StripDsp& strip = dsp_->channels[static_cast<std::size_t>(ch)];
    strip.eqEnabled = !bypassed;
    if (bypassed) {
        strip.eq.reset();
    }
}

void MixerEngine::setOutputEqBypassed(int out, bool bypassed) {
    if (dsp_ == nullptr) {
        return;
    }
    std::scoped_lock lock(dsp_->mutex);
    if (out < 0 || static_cast<std::size_t>(out) >= dsp_->outputs.size()) {
        return;
    }
    StripDsp& strip = dsp_->outputs[static_cast<std::size_t>(out)];
    strip.eqEnabled = !bypassed;
    if (bypassed) {
        strip.eq.reset();
    }
}

void MixerEngine::render(std::span<const AudioBlock> channelInputs,
                         std::span<AudioBlock> outputBlocks) noexcept {
    const std::size_t numChannels = std::min(channelInputs.size(), channels_.size());
    const std::size_t numOutputs = std::min(outputBlocks.size(), outputCount_);
    const int maxFrames = maxBlockFrames_;
    if (maxFrames <= 0 || numChannels == 0 || numOutputs == 0) {
        // The caller hands these blocks over expecting to write. Silencing
        // them here stops whatever was in the buffer from playing on.
        for (AudioBlock& block : outputBlocks) {
            if (block.channels == nullptr || block.numFrames <= 0) {
                continue;
            }
            for (int c = 0; c < block.numChannels; ++c) {
                if (block.channels[c] != nullptr) {
                    std::fill(block.channels[c], block.channels[c] + block.numFrames, 0.0f);
                }
            }
        }
        return;
    }

    for (std::size_t o = 0; o < numOutputs; ++o) {
        const int frames = clampFrames(outputBlocks[o].numFrames, maxFrames);
        float* accLeft = accumulators_.data() + (o * 2) * static_cast<std::size_t>(maxFrames);
        float* accRight = accLeft + maxFrames;
        for (int i = 0; i < frames; ++i) {
            accLeft[i] = 0.0f;
            accRight[i] = 0.0f;
        }
    }

    int blockFrames = 0;
    for (std::size_t c = 0; c < numChannels; ++c) {
        const int frames = clampFrames(channelInputs[c].numFrames, maxFrames);
        if (frames > blockFrames) {
            blockFrames = frames;
        }
    }
    for (std::size_t o = 0; o < numOutputs; ++o) {
        const int frames = clampFrames(outputBlocks[o].numFrames, maxFrames);
        if (frames > blockFrames) {
            blockFrames = frames;
        }
    }
    meters_.advance(blockFrames, sampleRate_);

    auto pushTapKind = [&](std::vector<TapState>& taps, Tap::Kind kind,
                           std::uint32_t itemId, float* left, float* right, int frames) {
        for (TapState& state : taps) {
            if (state.tap.kind == kind &&
                (kind != Tap::Kind::Item || state.tap.itemId == itemId)) {
                state.ring.push(left, right, frames);
            }
        }
    };

    for (std::size_t c = 0; c < numChannels; ++c) {
        const AudioBlock& input = channelInputs[c];
        const ChannelCache& cache = channels_[c];
        const int frames = clampFrames(input.numFrames, maxFrames);
        float* scratchLeft = scratch_.data() + (c * 2) * static_cast<std::size_t>(maxFrames);
        float* scratchRight = scratchLeft + maxFrames;
        const bool silenced = cache.muted || (anySolo_ && !cache.soloed) ||
                                (c < stoppedChannels_.size() && stoppedChannels_[c] != 0);
        const bool stopped = c < stoppedChannels_.size() && stoppedChannels_[c] != 0;
        const bool isMono = input.numChannels == 1;
        const bool isStereo = input.numChannels == 2;
        const bool hasMono =
            isMono && input.channels != nullptr && input.channels[0] != nullptr;
        const bool hasStereo = isStereo && input.channels != nullptr &&
                               input.channels[0] != nullptr && input.channels[1] != nullptr;
        if (hasMono) {
            // Mono-block upmix is intentional per spec section 3.2.
            const float* mono = input.channels[0];
            for (int i = 0; i < frames; ++i) {
                scratchLeft[i] = mono[i];
                scratchRight[i] = mono[i];
            }
        } else if (hasStereo) {
            const float* inLeft = input.channels[0];
            const float* inRight = input.channels[1];
            for (int i = 0; i < frames; ++i) {
                scratchLeft[i] = inLeft[i];
                scratchRight[i] = inRight[i];
            }
        } else {
            for (int i = 0; i < frames; ++i) {
                scratchLeft[i] = 0.0f;
                scratchRight[i] = 0.0f;
            }
        }
        // A stopped channel that already has silence at its input has nothing
        // to compute. Skip the strip and the VST run, but still advance every
        // tap the channel owns so the pre-fader timelines of routes and sends
        // stay aligned; scratch is all zeros here, so these are the same
        // values the per-step taps would have seen. Muted-but-not-stopped
        // channels keep running: their pre-fader taps must still pass audio.
        bool idleStopped = stopped;
        if (idleStopped) {
            for (int i = 0; i < frames; ++i) {
                if (scratchLeft[i] != 0.0f || scratchRight[i] != 0.0f) {
                    idleStopped = false;
                    break;
                }
            }
        }
        if (idleStopped) {
            if (c < channelTaps_.size()) {
                for (TapState& state : channelTaps_[c]) {
                    state.ring.push(scratchLeft, scratchRight, frames);
                }
            }
            if (c < channelIds_.size()) {
                meters_.observeChannelRealtime(channelIds_[c], 0.0f);
            }
            continue;
        }
        if (c < channelTaps_.size()) {
            pushTapKind(channelTaps_[c], Tap::Kind::Input, 0,
                        scratchLeft, scratchRight, frames);
        }
        // The strip's EQ and built-in chain are pre-fader and run before
        // inserts and gain/pan. The lock only spans the run so bypass and
        // band swaps never race a live filter.
        if (dsp_ != nullptr && c < dsp_->channels.size()) {
            TapPush push{c < channelTaps_.size() ? &channelTaps_[c] : nullptr,
                         scratchLeft, scratchRight, frames};
            std::scoped_lock lock(dsp_->mutex);
            processStrip(dsp_->channels[c], scratchLeft, scratchRight, frames,
                         c < channelTaps_.size() ? &MixerEngine::pushItemTap : nullptr,
                         &push);
        }
        // The built-in chain is an insert too: quarantine anything it emitted
        // before the VST chain and the fader see it.
        if (scrubEnabled_) {
            scrubbedSamples_ += static_cast<std::uint64_t>(
                scrubNonFinite(scratchLeft, scratchRight, frames));
        }
        if (c < insertChains_.size() && insertChains_[c] != nullptr) {
            float* scratchPtrs[2] = {scratchLeft, scratchRight};
            AudioBlock scratchBlock{scratchPtrs, 2, frames};
            TapPush push{c < channelTaps_.size() ? &channelTaps_[c] : nullptr,
                         scratchLeft, scratchRight, frames};
            insertChains_[c]->process(
                scratchBlock,
                c < channelTaps_.size() ? &MixerEngine::pushInsertTap : nullptr, &push);
            // The chain scrubs after each VST; scrub the strip boundary as
            // well so a plugin that wrote past its block cannot leak.
            if (scrubEnabled_) {
                scrubbedSamples_ += static_cast<std::uint64_t>(
                    scrubNonFinite(scratchLeft, scratchRight, frames));
            }
        }
        if (silenced || cache.gain == 0.0f) {
            for (int i = 0; i < frames; ++i) {
                scratchLeft[i] = 0.0f;
                scratchRight[i] = 0.0f;
            }
        } else {
            const float leftScale = cache.gain * cache.panLeft;
            const float rightScale = cache.gain * cache.panRight;
            for (int i = 0; i < frames; ++i) {
                scratchLeft[i] *= leftScale;
                scratchRight[i] *= rightScale;
            }
        }
        if (c < channelTaps_.size()) {
            pushTapKind(channelTaps_[c], Tap::Kind::Output, 0,
                        scratchLeft, scratchRight, frames);
        }
        float channelPeak = 0.0f;
        for (int i = 0; i < frames; ++i) {
            const float left = std::fabs(scratchLeft[i]);
            const float right = std::fabs(scratchRight[i]);
            if (left > channelPeak) {
                channelPeak = left;
            }
            if (right > channelPeak) {
                channelPeak = right;
            }
        }
        if (c < channelIds_.size()) {
            meters_.observeChannelRealtime(channelIds_[c], channelPeak);
        }
    }

    for (const RouteCache& route : routes_) {
        if (route.channelIndex >= numChannels || route.outputIndex >= numOutputs) {
            continue;
        }
        if (route.sendGainL == 0.0f && route.sendGainR == 0.0f) {
            continue;
        }
        const int channelFrames =
            clampFrames(channelInputs[route.channelIndex].numFrames, maxFrames);
        const int outputFrames =
            clampFrames(outputBlocks[route.outputIndex].numFrames, maxFrames);
        const int frames = channelFrames < outputFrames ? channelFrames : outputFrames;
        const float* scratchLeft =
            scratch_.data() + (route.channelIndex * 2) * static_cast<std::size_t>(maxFrames);
        const float* scratchRight = scratchLeft + maxFrames;
        const bool tapped = route.channelIndex < channelTaps_.size() &&
                            (route.delaySamples > 0 ||
                             channelTaps_[route.channelIndex][route.tapIndex].tap.kind !=
                                 Tap::Kind::Output);
        if (tapped) {
            float* tapLeft = tapScratch_.data();
            float* tapRight = tapLeft + maxFrames;
            channelTaps_[route.channelIndex][route.tapIndex].ring.read(
                route.delaySamples, tapLeft, tapRight, frames);
            scratchLeft = tapLeft;
            scratchRight = tapRight;
        }
        float* accLeft =
            accumulators_.data() + (route.outputIndex * 2) * static_cast<std::size_t>(maxFrames);
        float* accRight = accLeft + maxFrames;
        for (int i = 0; i < frames; ++i) {
            accLeft[i] += scratchLeft[i] * route.sendGainL;
            accRight[i] += scratchRight[i] * route.sendGainR;
        }
    }

    // Buses: sum their channel sends from the post-fader scratch buffers, run
    // the bus strip, inserts and fader, then add the result to the outputs.
    // Side-chain taps are a live-engine feature; this path is audio only.
    const std::size_t numBuses = buses_.size();
    for (std::size_t b = 0; b < numBuses; ++b) {
        float* busLeft =
            busBuffers_.data() + (b * 2) * static_cast<std::size_t>(maxFrames);
        float* busRight = busLeft + maxFrames;
        for (int i = 0; i < blockFrames; ++i) {
            busLeft[i] = 0.0f;
            busRight[i] = 0.0f;
        }
    }
    for (const BusSendCache& send : busSends_) {
        if (send.channelIndex >= numChannels || send.busIndex >= numBuses) {
            continue;
        }
        if (send.sendGainL == 0.0f && send.sendGainR == 0.0f) {
            continue;
        }
        const int channelFrames =
            clampFrames(channelInputs[send.channelIndex].numFrames, maxFrames);
        const int frames = channelFrames < blockFrames ? channelFrames : blockFrames;
        const float* scratchLeft =
            scratch_.data() + (send.channelIndex * 2) * static_cast<std::size_t>(maxFrames);
        const float* scratchRight = scratchLeft + maxFrames;
        const bool tapped = send.channelIndex < channelTaps_.size() &&
                            (send.delaySamples > 0 ||
                             channelTaps_[send.channelIndex][send.tapIndex].tap.kind !=
                                 Tap::Kind::Output);
        if (tapped) {
            float* tapLeft = tapScratch_.data();
            float* tapRight = tapLeft + maxFrames;
            channelTaps_[send.channelIndex][send.tapIndex].ring.read(
                send.delaySamples, tapLeft, tapRight, frames);
            scratchLeft = tapLeft;
            scratchRight = tapRight;
        }
        float* busLeft =
            busBuffers_.data() + (send.busIndex * 2) * static_cast<std::size_t>(maxFrames);
        float* busRight = busLeft + maxFrames;
        for (int i = 0; i < frames; ++i) {
            busLeft[i] += scratchLeft[i] * send.sendGainL;
            busRight[i] += scratchRight[i] * send.sendGainR;
        }
    }
    if (dsp_ != nullptr) {
        std::scoped_lock lock(dsp_->mutex);
        for (std::size_t b = 0; b < numBuses; ++b) {
            if (b >= dsp_->buses.size()) {
                continue;
            }
            float* busLeft =
                busBuffers_.data() + (b * 2) * static_cast<std::size_t>(maxFrames);
            float* busRight = busLeft + maxFrames;
            const float trim = b < buses_.size() ? buses_[b].trim : 1.0f;
            if (trim != 1.0f) {
                for (int i = 0; i < blockFrames; ++i) {
                    busLeft[i] *= trim;
                    busRight[i] *= trim;
                }
            }
            processStrip(dsp_->buses[b], busLeft, busRight, blockFrames);
            if (scrubEnabled_) {
                scrubbedSamples_ += static_cast<std::uint64_t>(
                    scrubNonFinite(busLeft, busRight, blockFrames));
            }
        }
    }
    for (std::size_t b = 0; b < numBuses; ++b) {
        if (b >= busInsertChains_.size() || busInsertChains_[b] == nullptr) {
            continue;
        }
        float* busLeft =
            busBuffers_.data() + (b * 2) * static_cast<std::size_t>(maxFrames);
        float* busRight = busLeft + maxFrames;
        float* chainPtrs[2] = {busLeft, busRight};
        AudioBlock busBlock{chainPtrs, 2, blockFrames};
        busInsertChains_[b]->process(busBlock);
        if (scrubEnabled_) {
            scrubbedSamples_ += static_cast<std::uint64_t>(
                scrubNonFinite(busLeft, busRight, blockFrames));
        }
    }
    for (std::size_t b = 0; b < numBuses; ++b) {
        const BusCache& cache = buses_[b];
        float* busLeft =
            busBuffers_.data() + (b * 2) * static_cast<std::size_t>(maxFrames);
        float* busRight = busLeft + maxFrames;
        const float gain = cache.muted ? 0.0f : cache.gain;
        float busPeak = 0.0f;
        for (int i = 0; i < blockFrames; ++i) {
            busLeft[i] *= gain;
            busRight[i] *= gain;
            const float left = std::fabs(busLeft[i]);
            const float right = std::fabs(busRight[i]);
            if (left > busPeak) {
                busPeak = left;
            }
            if (right > busPeak) {
                busPeak = right;
            }
        }
        if (b < busIds_.size()) {
            meters_.observeBusRealtime(busIds_[b], busPeak);
        }
        if (b < busCompensation_.size()) {
            busCompensation_[b].push(busLeft, busRight, blockFrames);
        }
    }
    for (const BusRouteCache& route : busRoutes_) {
        if (route.busIndex >= numBuses || route.outputIndex >= numOutputs) {
            continue;
        }
        if (route.sendGainL == 0.0f && route.sendGainR == 0.0f) {
            continue;
        }
        const int outputFrames =
            clampFrames(outputBlocks[route.outputIndex].numFrames, maxFrames);
        const int frames = outputFrames < blockFrames ? outputFrames : blockFrames;
        const float* busLeft =
            busBuffers_.data() + (route.busIndex * 2) * static_cast<std::size_t>(maxFrames);
        const float* busRight = busLeft + maxFrames;
        if (route.delaySamples > 0 && route.busIndex < busCompensation_.size()) {
            float* tapLeft = tapScratch_.data();
            float* tapRight = tapLeft + maxFrames;
            busCompensation_[route.busIndex].read(
                route.delaySamples, tapLeft, tapRight, frames);
            busLeft = tapLeft;
            busRight = tapRight;
        }
        float* accLeft = accumulators_.data() +
                         (route.outputIndex * 2) * static_cast<std::size_t>(maxFrames);
        float* accRight = accLeft + maxFrames;
        for (int i = 0; i < frames; ++i) {
            accLeft[i] += busLeft[i] * route.sendGainL;
            accRight[i] += busRight[i] * route.sendGainR;
        }
    }

    // Output EQ is pre-master-write and runs before the output insert
    // chains, mirroring the channel tap. Bypassed outputs are untouched.
    if (dsp_ != nullptr) {
        std::scoped_lock lock(dsp_->mutex);
        for (std::size_t o = 0; o < numOutputs; ++o) {
            const int frames = clampFrames(outputBlocks[o].numFrames, maxFrames);
            float* accLeft =
                accumulators_.data() + (o * 2) * static_cast<std::size_t>(maxFrames);
            float* accRight = accLeft + maxFrames;
            if (o < dsp_->outputs.size()) {
                processStrip(dsp_->outputs[o], accLeft, accRight, frames);
            }
            if (scrubEnabled_) {
                scrubbedSamples_ += static_cast<std::uint64_t>(
                    scrubNonFinite(accLeft, accRight, frames));
            }
        }
    }

    for (std::size_t o = 0; o < numOutputs; ++o) {
        if (o >= outputInsertChains_.size() || outputInsertChains_[o] == nullptr) {
            continue;
        }
        const int frames = clampFrames(outputBlocks[o].numFrames, maxFrames);
        float* accLeft =
            accumulators_.data() + (o * 2) * static_cast<std::size_t>(maxFrames);
        float* accRight = accLeft + maxFrames;
        float* chainPtrs[2] = {accLeft, accRight};
        AudioBlock chainBlock{chainPtrs, 2, frames};
        outputInsertChains_[o]->process(chainBlock);
        if (scrubEnabled_) {
            scrubbedSamples_ += static_cast<std::uint64_t>(
                scrubNonFinite(accLeft, accRight, frames));
        }
    }

    for (std::size_t o = 0; o < numOutputs; ++o) {
        AudioBlock& output = outputBlocks[o];
        const int frames = clampFrames(output.numFrames, maxFrames);
        const float* accLeft =
            accumulators_.data() + (o * 2) * static_cast<std::size_t>(maxFrames);
        const float* accRight = accLeft + maxFrames;
        const bool outMuted = o < outputMuted_.size() && outputMuted_[o] != 0;
        const float outGain =
            outMuted ? 0.0f : (o < outputGains_.size() ? outputGains_[o] : 1.0f);
        float outputPeak = 0.0f;
        for (int i = 0; i < frames; ++i) {
            const float left = std::fabs(accLeft[i] * outGain);
            const float right = std::fabs(accRight[i] * outGain);
            if (left > outputPeak) {
                outputPeak = left;
            }
            if (right > outputPeak) {
                outputPeak = right;
            }
        }
        if (o < outputIds_.size()) {
            meters_.observeOutputRealtime(outputIds_[o], outputPeak);
        }
        if (output.channels == nullptr) {
            continue;
        }
        const bool disabled = o < disabledOutputs_.size() && disabledOutputs_[o] != 0;
        if (output.numChannels >= 1 && output.channels[0] != nullptr) {
            if (disabled || outGain == 0.0f) {
                for (int i = 0; i < frames; ++i) {
                    output.channels[0][i] = 0.0f;
                }
            } else {
                for (int i = 0; i < frames; ++i) {
                    output.channels[0][i] = accLeft[i] * outGain;
                }
            }
        }
        if (output.numChannels >= 2 && output.channels[1] != nullptr) {
            if (disabled || outGain == 0.0f) {
                for (int i = 0; i < frames; ++i) {
                    output.channels[1][i] = 0.0f;
                }
            } else {
                for (int i = 0; i < frames; ++i) {
                    output.channels[1][i] = accRight[i] * outGain;
                }
            }
        }
        // A wide device (the WASAPI probe cap defaults to 8 channels) also
        // receives channels this stereo mix never writes. Clear them: leaving
        // the host buffer's old contents there plays unrelated audio.
        for (int c = 2; c < output.numChannels; ++c) {
            if (output.channels[c] != nullptr) {
                for (int i = 0; i < frames; ++i) {
                    output.channels[c][i] = 0.0f;
                }
            }
        }
    }
}

std::vector<std::uint32_t> MixerEngine::failedPluginSlots() const {
    std::vector<std::uint32_t> failed;
    for (const std::unique_ptr<InsertChain>& chain : insertChains_) {
        if (chain != nullptr) {
            const std::vector<std::uint32_t> chainFailures = chain->failedSlots();
            failed.insert(failed.end(), chainFailures.begin(), chainFailures.end());
        }
    }
    for (const std::unique_ptr<InsertChain>& chain : outputInsertChains_) {
        if (chain != nullptr) {
            const std::vector<std::uint32_t> chainFailures = chain->failedSlots();
            failed.insert(failed.end(), chainFailures.begin(), chainFailures.end());
        }
    }
    for (const std::unique_ptr<InsertChain>& chain : busInsertChains_) {
        if (chain != nullptr) {
            const std::vector<std::uint32_t> chainFailures = chain->failedSlots();
            failed.insert(failed.end(), chainFailures.begin(), chainFailures.end());
        }
    }
    return failed;
}

std::uint64_t MixerEngine::scrubbedSamples() const {
    std::uint64_t total = scrubbedSamples_;
    for (const std::unique_ptr<InsertChain>& chain : insertChains_) {
        if (chain != nullptr) total += chain->scrubbedSamples();
    }
    for (const std::unique_ptr<InsertChain>& chain : outputInsertChains_) {
        if (chain != nullptr) total += chain->scrubbedSamples();
    }
    for (const std::unique_ptr<InsertChain>& chain : busInsertChains_) {
        if (chain != nullptr) total += chain->scrubbedSamples();
    }
    return total;
}

const MeterBank& MixerEngine::meters() const noexcept { return meters_; }

} // namespace pluginrack
