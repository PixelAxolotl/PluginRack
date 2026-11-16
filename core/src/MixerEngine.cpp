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

    const int maxFrames = maxBlockFrames < 0 ? 0 : maxBlockFrames;
    maxBlockFrames_ = maxFrames;
    outputCount_ = scene.outputs.size();
    anySolo_ = anySolo;
    sampleRate_ = scene.sampleRate;

    const std::size_t stereoFrames = static_cast<std::size_t>(maxFrames) * 2;
    scratch_.assign(channels_.size() * stereoFrames, 0.0f);
    accumulators_.assign(outputCount_ * stereoFrames, 0.0f);

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

    dsp_ = std::make_unique<DspState>();
    dsp_->channels.reserve(scene.channels.size());
    for (const Channel& channel : scene.channels) {
        dsp_->channels.push_back(buildStrip(channel, scene.sampleRate, maxFrames));
    }
    dsp_->outputs.reserve(scene.outputs.size());
    for (const Output& output : scene.outputs) {
        dsp_->outputs.push_back(buildStrip(output, scene.sampleRate, maxFrames));
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
    meters_.setTargets(channelIds_.data(), channelIds_.size(),
                       outputIds_.data(), outputIds_.size());
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

template <typename StripScene>
MixerEngine::StripDsp MixerEngine::buildStrip(const StripScene& source, double sampleRate, int maxFrames) {
    StripDsp strip;
    strip.eq.prepare(sampleRate, maxFrames);
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

    // Resolve the running order once. The scene's chain names effects by id;
    // anything the chain leaves out still runs, appended in the historical
    // Dynamics -> Multiband -> DeEsser -> Limiter order so old scenes render
    // exactly as before. EQ runs before the chain and inserts render through
    // their own InsertChain, so both are skipped here.
    std::vector<char> usedDynamics(source.effects.size(), 0);
    std::vector<char> usedMultiband(source.multibandEffects.size(), 0);
    std::vector<char> usedDeEssers(source.deEsserEffects.size(), 0);
    std::vector<char> usedLimiters(source.limiterEffects.size(), 0);
    for (const ChainItem& item : source.chain) {
        switch (item.type) {
        case ChainItemType::Dynamics:
            if (const auto index = findByEffectId(source.effects, item.id)) {
                usedDynamics[*index] = 1;
                strip.steps.push_back({item.type, *index});
            }
            break;
        case ChainItemType::Multiband:
            if (const auto index = findByEffectId(source.multibandEffects, item.id)) {
                usedMultiband[*index] = 1;
                strip.steps.push_back({item.type, *index});
            }
            break;
        case ChainItemType::DeEsser:
            if (const auto index = findByEffectId(source.deEsserEffects, item.id)) {
                usedDeEssers[*index] = 1;
                strip.steps.push_back({item.type, *index});
            }
            break;
        case ChainItemType::Limiter:
            if (const auto index = findByEffectId(source.limiterEffects, item.id)) {
                usedLimiters[*index] = 1;
                strip.steps.push_back({item.type, *index});
            }
            break;
        case ChainItemType::Eq:
        case ChainItemType::Insert:
            break;
        }
    }
    for (std::size_t i = 0; i < usedDynamics.size(); ++i) {
        if (usedDynamics[i] == 0) strip.steps.push_back({ChainItemType::Dynamics, i});
    }
    for (std::size_t i = 0; i < usedMultiband.size(); ++i) {
        if (usedMultiband[i] == 0) strip.steps.push_back({ChainItemType::Multiband, i});
    }
    for (std::size_t i = 0; i < usedDeEssers.size(); ++i) {
        if (usedDeEssers[i] == 0) strip.steps.push_back({ChainItemType::DeEsser, i});
    }
    for (std::size_t i = 0; i < usedLimiters.size(); ++i) {
        if (usedLimiters[i] == 0) strip.steps.push_back({ChainItemType::Limiter, i});
    }
    return strip;
}

void MixerEngine::processStrip(StripDsp& strip, float* left, float* right, int frames) {
    if (strip.eqEnabled) {
        flushNonFinite(left, right, frames);
        strip.eq.process(left, right, frames);
    }
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
        case ChainItemType::Eq:
        case ChainItemType::Insert:
            break;
        }
    }
}

void MixerEngine::setChannelEq(int ch, const std::vector<EqBand>& bands) {
    if (dsp_ == nullptr) {
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

    for (std::size_t c = 0; c < numChannels; ++c) {
        const AudioBlock& input = channelInputs[c];
        const ChannelCache& cache = channels_[c];
        const int frames = clampFrames(input.numFrames, maxFrames);
        float* scratchLeft = scratch_.data() + (c * 2) * static_cast<std::size_t>(maxFrames);
        float* scratchRight = scratchLeft + maxFrames;
        const bool silenced = cache.muted || (anySolo_ && !cache.soloed) ||
                                (c < stoppedChannels_.size() && stoppedChannels_[c] != 0);
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
        // The strip's EQ and built-in chain are pre-fader and run before
        // inserts and gain/pan. The lock only spans the run so bypass and
        // band swaps never race a live filter.
        if (dsp_ != nullptr && c < dsp_->channels.size()) {
            std::scoped_lock lock(dsp_->mutex);
            processStrip(dsp_->channels[c], scratchLeft, scratchRight, frames);
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
            insertChains_[c]->process(scratchBlock);
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
        float* accLeft =
            accumulators_.data() + (route.outputIndex * 2) * static_cast<std::size_t>(maxFrames);
        float* accRight = accLeft + maxFrames;
        for (int i = 0; i < frames; ++i) {
            accLeft[i] += scratchLeft[i] * route.sendGainL;
            accRight[i] += scratchRight[i] * route.sendGainR;
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
    }
}

int MixerEngine::channelCount() const noexcept { return static_cast<int>(channels_.size()); }

int MixerEngine::outputCount() const noexcept { return static_cast<int>(outputCount_); }

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
    return total;
}

const MeterBank& MixerEngine::meters() const noexcept { return meters_; }

} // namespace pluginrack
