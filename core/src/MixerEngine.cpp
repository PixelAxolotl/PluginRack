#include "pluginrack/MixerEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

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

} // namespace

MixerEngine::MixerEngine() : eqState_(std::make_unique<EqState>()) {}

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
        cache.gain = dbToLinear(channel.gainDb);
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

    routes_.clear();
    routes_.reserve(scene.routes.size());
    for (const Route& route : scene.routes) {
        RouteCache cache;
        cache.channelIndex = channelIndex[route.channelId];
        cache.outputIndex = outputIndex[route.outputId];
        cache.sendGain = dbToLinear(route.sendGainDb);
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
                channel.inserts, *pluginFactory_, scene.sampleRate, maxFrames));
        }
    }
    outputInsertChains_.clear();
    if (pluginFactory_ != nullptr) {
        outputInsertChains_.reserve(scene.outputs.size());
        for (const Output& output : scene.outputs) {
            outputInsertChains_.push_back(std::make_unique<InsertChain>(
                output.inserts, *pluginFactory_, scene.sampleRate, maxFrames));
        }
    }

    eqState_ = std::make_unique<EqState>();
    eqState_->channelEqs.reserve(scene.channels.size());
    eqState_->channelEqEnabled.reserve(scene.channels.size());
    for (const Channel& channel : scene.channels) {
        EqEngine eq;
        eq.prepare(scene.sampleRate, maxFrames);
        eq.setBands(channel.eqBands);
        eqState_->channelEqs.push_back(std::move(eq));
        eqState_->channelEqEnabled.push_back(channel.eqEnabled ? 1 : 0);
    }
    eqState_->outputEqs.reserve(scene.outputs.size());
    eqState_->outputEqEnabled.reserve(scene.outputs.size());
    for (const Output& output : scene.outputs) {
        EqEngine eq;
        eq.prepare(scene.sampleRate, maxFrames);
        eq.setBands(output.eqBands);
        eqState_->outputEqs.push_back(std::move(eq));
        eqState_->outputEqEnabled.push_back(output.eqEnabled ? 1 : 0);
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
    meters_.reset();
    for (const std::uint32_t id : channelIds_) {
        meters_.observeChannel(id, 0.0f);
    }
    for (const std::uint32_t id : outputIds_) {
        meters_.observeOutput(id, 0.0f);
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

void MixerEngine::setChannelEq(int ch, const std::vector<EqBand>& bands) {
    if (eqState_ == nullptr) {
        return;
    }
    std::scoped_lock lock(eqState_->mutex);
    if (ch < 0 || static_cast<std::size_t>(ch) >= eqState_->channelEqs.size()) {
        return;
    }
    eqState_->channelEqs[static_cast<std::size_t>(ch)].setBands(bands);
}

void MixerEngine::setOutputEq(int out, const std::vector<EqBand>& bands) {
    if (eqState_ == nullptr) {
        return;
    }
    std::scoped_lock lock(eqState_->mutex);
    if (out < 0 || static_cast<std::size_t>(out) >= eqState_->outputEqs.size()) {
        return;
    }
    eqState_->outputEqs[static_cast<std::size_t>(out)].setBands(bands);
}

void MixerEngine::setChannelEqBypassed(int ch, bool bypassed) {
    if (eqState_ == nullptr) {
        return;
    }
    std::scoped_lock lock(eqState_->mutex);
    if (ch < 0 || static_cast<std::size_t>(ch) >= eqState_->channelEqEnabled.size()) {
        return;
    }
    eqState_->channelEqEnabled[static_cast<std::size_t>(ch)] = bypassed ? 0 : 1;
    if (bypassed) {
        eqState_->channelEqs[static_cast<std::size_t>(ch)].reset();
    }
}

void MixerEngine::setOutputEqBypassed(int out, bool bypassed) {
    if (eqState_ == nullptr) {
        return;
    }
    std::scoped_lock lock(eqState_->mutex);
    if (out < 0 || static_cast<std::size_t>(out) >= eqState_->outputEqEnabled.size()) {
        return;
    }
    eqState_->outputEqEnabled[static_cast<std::size_t>(out)] = bypassed ? 0 : 1;
    if (bypassed) {
        eqState_->outputEqs[static_cast<std::size_t>(out)].reset();
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
        // Channel EQ is pre-fader and runs before inserts and gain/pan.
        // Bypassed strips skip process() entirely, keeping the output
        // bit-identical to the pre-EQ engine. The lock only spans flag
        // read + process() so bypass/band swaps never race a live filter.
        if (eqState_ != nullptr) {
            std::scoped_lock lock(eqState_->mutex);
            const std::size_t n = eqState_->channelEqEnabled.size();
            if (c < n && eqState_->channelEqEnabled[c] != 0 &&
                c < eqState_->channelEqs.size()) {
                flushNonFinite(scratchLeft, scratchRight, frames);
                eqState_->channelEqs[c].process(scratchLeft, scratchRight, frames);
            }
        }
        if (c < insertChains_.size() && insertChains_[c] != nullptr) {
            float* scratchPtrs[2] = {scratchLeft, scratchRight};
            AudioBlock scratchBlock{scratchPtrs, 2, frames};
            insertChains_[c]->process(scratchBlock);
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
            meters_.observeChannel(channelIds_[c], channelPeak);
        }
    }

    for (const RouteCache& route : routes_) {
        if (route.channelIndex >= numChannels || route.outputIndex >= numOutputs) {
            continue;
        }
        if (route.sendGain == 0.0f) {
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
            accLeft[i] += scratchLeft[i] * route.sendGain;
            accRight[i] += scratchRight[i] * route.sendGain;
        }
    }

    // Output EQ is pre-master-write and runs before the output insert
    // chains, mirroring the channel tap. Bypassed outputs are untouched.
    if (eqState_ != nullptr) {
        std::scoped_lock lock(eqState_->mutex);
        for (std::size_t o = 0; o < numOutputs; ++o) {
            if (o >= eqState_->outputEqEnabled.size() ||
                eqState_->outputEqEnabled[o] == 0 || o >= eqState_->outputEqs.size()) {
                continue;
            }
            const int frames = clampFrames(outputBlocks[o].numFrames, maxFrames);
            float* accLeft =
                accumulators_.data() + (o * 2) * static_cast<std::size_t>(maxFrames);
            float* accRight = accLeft + maxFrames;
            flushNonFinite(accLeft, accRight, frames);
            eqState_->outputEqs[o].process(accLeft, accRight, frames);
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
    }

    for (std::size_t o = 0; o < numOutputs; ++o) {
        AudioBlock& output = outputBlocks[o];
        const int frames = clampFrames(output.numFrames, maxFrames);
        const float* accLeft =
            accumulators_.data() + (o * 2) * static_cast<std::size_t>(maxFrames);
        const float* accRight = accLeft + maxFrames;
        float outputPeak = 0.0f;
        for (int i = 0; i < frames; ++i) {
            const float left = std::fabs(accLeft[i]);
            const float right = std::fabs(accRight[i]);
            if (left > outputPeak) {
                outputPeak = left;
            }
            if (right > outputPeak) {
                outputPeak = right;
            }
        }
        if (o < outputIds_.size()) {
            meters_.observeOutput(outputIds_[o], outputPeak);
        }
        if (output.channels == nullptr) {
            continue;
        }
        const bool disabled = o < disabledOutputs_.size() && disabledOutputs_[o] != 0;
        if (output.numChannels >= 1 && output.channels[0] != nullptr) {
            if (disabled) {
                for (int i = 0; i < frames; ++i) {
                    output.channels[0][i] = 0.0f;
                }
            } else {
                for (int i = 0; i < frames; ++i) {
                    output.channels[0][i] = accLeft[i];
                }
            }
        }
        if (output.numChannels >= 2 && output.channels[1] != nullptr) {
            if (disabled) {
                for (int i = 0; i < frames; ++i) {
                    output.channels[1][i] = 0.0f;
                }
            } else {
                for (int i = 0; i < frames; ++i) {
                    output.channels[1][i] = accRight[i];
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

const MeterBank& MixerEngine::meters() const noexcept { return meters_; }

} // namespace pluginrack
