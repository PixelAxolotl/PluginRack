#include "AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <map>

AudioEngine::AudioEngine() {
    for (int i = 0; i < 3; ++i) addOutput();
    addChannel();
    if (!outputs.empty()) {
        setOutputDevice(outputs.front()->id, "System Default");
    }
    if (!channels.empty()) {
        setChannelInputDevice(channels.front()->id, "System Default");
    }
}

void AudioEngine::prepareToPlay(int maxBlockFrames) {
    const juce::ScopedLock lock(stateLock);
    maxBlockFrames_ = juce::jmax(1, maxBlockFrames);
    currentBufferSize = maxBlockFrames_;
    silenceBuffer_.setSize(2, maxBlockFrames_, false, true, true);
    silenceBuffer_.clear();
    for (auto& channel : channels) {
        channel->buffer.setSize(2, maxBlockFrames_, false, true, true);
        channel->buffer.clear();
        for (auto& insert : channel->inserts) {
            insert->pluginHost.prepareToPlay(currentSampleRate, currentBufferSize);
        }
        meters.observeChannel(channel->id, 0.0f);
    }
    for (auto& output : outputs) {
        output->buffer.setSize(2, maxBlockFrames_, false, true, true);
        output->buffer.clear();
        for (auto& insert : output->inserts) {
            insert->pluginHost.prepareToPlay(currentSampleRate, currentBufferSize);
        }
        meters.observeOutput(output->id, 0.0f);
    }
}

void AudioEngine::render(int frames) {
    if (frames <= 0) return;

    const juce::ScopedTryLock lock(stateLock);
    if (!lock.isLocked()) {
        for (auto& output : outputs) {
            if (output->output) {
                juce::AudioBuffer<float> silence(2, frames);
                silence.clear();
                output->output->push(silence, frames);
            }
        }
        return;
    }

    // Oversized block: silence it, count once, skip processing.
    if (maxBlockFrames_ > 0 && frames > maxBlockFrames_) {
        oversizedBlockCount_.fetch_add(1, std::memory_order_relaxed);
        for (auto& output : outputs) {
            if (output->output) {
                juce::AudioBuffer<float> silence(2, frames);
                silence.clear();
                output->output->push(silence, frames);
            }
        }
        outputSampleCounter_ += (std::uint64_t)frames;
        return;
    }

    // buffers are preallocated in prepareToPlay; clear used range only.
    for (auto& channel : channels) {
        channel->buffer.clear(0, frames);
        if (channel->input) {
            channel->input->pull(channel->buffer, frames, channel->inputChannels);
        }
    }
    for (auto& output : outputs) {
        output->buffer.clear(0, frames);
    }

    meters.advance(frames, currentSampleRate);
    bool anySolo = false;
    for (const auto& channel : channels) {
        anySolo = anySolo || channel->soloed.get();
    }

    for (auto& channel : channels) {
        juce::AudioBuffer<float> block(channel->buffer.getArrayOfWritePointers(), 2, 0, frames);
        for (auto& insert : channel->inserts) {
            insert->pluginHost.processBlock(block, insert->bypassed.get());
        }

        const bool silent = channel->muted.get() || (anySolo && !channel->soloed.get());
        const float gain = silent ? 0.0f : decibelsToGain(channel->gainDb.get() + channel->trimDb.get());
        const float theta = (channel->pan.get() + 1.0f) * juce::MathConstants<float>::pi / 4.0f;
        channel->buffer.applyGain(0, 0, frames, gain * std::cos(theta));
        channel->buffer.applyGain(1, 0, frames, gain * std::sin(theta));

        const float peak = juce::jmax(channel->buffer.getMagnitude(0, 0, frames),
                                      channel->buffer.getMagnitude(1, 0, frames));
        meters.observeChannel(channel->id, peak);
        channel->peak = meters.channelPeak(channel->id);
    }

    for (const auto& route : routes) {
        const auto channelIt = std::find_if(channels.begin(), channels.end(), [&route](const auto& c) {
            return c->id == route->channelId;
        });
        const auto outputIt = std::find_if(outputs.begin(), outputs.end(), [&route](const auto& o) {
            return o->id == route->outputId;
        });
        if (channelIt == channels.end() || outputIt == outputs.end()) {
            continue;
        }

        const float sendGainL = decibelsToGain(route->sendGainDbL.get());
        const float sendGainR = decibelsToGain(route->sendGainDbR.get());
        if (sendGainL == 0.0f && sendGainR == 0.0f) {
            continue;
        }
        if (sendGainL != 0.0f) {
            (*outputIt)->buffer.addFrom(0, 0, (*channelIt)->buffer, 0, 0, frames, sendGainL);
        }
        if (sendGainR != 0.0f) {
            (*outputIt)->buffer.addFrom(1, 0, (*channelIt)->buffer, 1, 0, frames, sendGainR);
        }
    }

    for (auto& output : outputs) {
        juce::AudioBuffer<float> block(output->buffer.getArrayOfWritePointers(), 2, 0, frames);
        for (auto& insert : output->inserts) {
            insert->pluginHost.processBlock(block, insert->bypassed.get());
        }
        if (output->flipped.get()) {
            for (int i = 0; i < frames; ++i) {
                const float l = output->buffer.getSample(0, i);
                const float r = output->buffer.getSample(1, i);
                output->buffer.setSample(0, i, r);
                output->buffer.setSample(1, i, l);
            }
        }
        if (output->layout == pluginrack::ChannelLayout::Mono) {
            for (int i = 0; i < frames; ++i) {
                const float v = output->buffer.getSample(0, i);
                output->buffer.setSample(1, i, v);
            }
        }
    }

    for (auto& output : outputs) {
        const float outGain = output->muted.get() ? 0.0f : decibelsToGain(output->gainDb.get());
        if (outGain != 1.0f) {
            output->buffer.applyGain(0, 0, frames, outGain);
            output->buffer.applyGain(1, 0, frames, outGain);
        }
        if (output->muted.get()) {
            output->buffer.clear(0, frames);
        }
        const float peak = juce::jmax(output->buffer.getMagnitude(0, 0, frames),
                                      output->buffer.getMagnitude(1, 0, frames));
        meters.observeOutput(output->id, peak);
        output->peak = meters.outputPeak(output->id);
    }

    // Round-trip latency measurement: inject a one-sample impulse on the
    // clock output after all processing, then detect it on input.
    if (latencyMeasureActive_.load(std::memory_order_relaxed) &&
        !latencyMeasureEmitted_.load(std::memory_order_relaxed)) {
        for (auto& output : outputs) {
            if (output->id == clockOutputId && output->output != nullptr) {
                output->buffer.setSample(0, 0, 1.0f);
                output->buffer.setSample(1, 0, 1.0f);
            }
        }
        latencyMeasureOutputSample_.store(outputSampleCounter_, std::memory_order_relaxed);
        latencyMeasureStartMs_.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
        latencyMeasureEmitted_.store(true, std::memory_order_relaxed);
    }

    for (auto& output : outputs) {
        if (output->output) {
            output->output->push(output->buffer, frames);
        }
    }

    if (latencyMeasureActive_.load(std::memory_order_relaxed)) {
        latencyMeasureBlocks_.fetch_add(1, std::memory_order_relaxed);
        bool detected = false;
        for (auto& entry : inputCaptures_) {
            if (entry.second.capture != nullptr &&
                entry.second.capture->isLatencyImpulseDetected()) {
                detected = true;
                break;
            }
        }
        if (detected) {
            const auto now = juce::Time::getMillisecondCounter();
            const auto start = latencyMeasureStartMs_.load(std::memory_order_relaxed);
            const std::uint32_t ms = (now >= start) ? (now - start) : 0;
            const double sr = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
            latencyMeasuredSamples_.store((int) (ms * sr / 1000.0), std::memory_order_relaxed);
            latencyMeasuredValid_.store(true, std::memory_order_relaxed);
            latencyMeasureActive_.store(false, std::memory_order_relaxed);
            for (auto& entry : inputCaptures_) {
                if (entry.second.capture != nullptr)
                    entry.second.capture->setLatencyMeasurementActive(false);
            }
        } else {
            const auto now = juce::Time::getMillisecondCounter();
            const auto start = latencyMeasureStartMs_.load(std::memory_order_relaxed);
            const bool timedOut = latencyMeasureEmitted_.load(std::memory_order_relaxed)
                && (now - start > 3000 || latencyMeasureBlocks_.load(std::memory_order_relaxed) > 200);
            if (timedOut) {
                latencyMeasureFailed_.store(true, std::memory_order_relaxed);
                latencyMeasureActive_.store(false, std::memory_order_relaxed);
                for (auto& entry : inputCaptures_) {
                    if (entry.second.capture != nullptr)
                        entry.second.capture->setLatencyMeasurementActive(false);
                }
            }
        }
    }

    outputSampleCounter_ += (std::uint64_t)frames;
}

void AudioEngine::releaseResources() {
    const juce::ScopedLock lock(stateLock);
    for (auto& channel : channels) {
        for (auto& insert : channel->inserts) {
            insert->pluginHost.releaseResources();
        }
    }
    for (auto& output : outputs) {
        for (auto& insert : output->inserts) {
            insert->pluginHost.releaseResources();
        }
    }
}

void AudioEngine::removeChannel(std::uint32_t channelId) {
    const juce::ScopedLock lock(stateLock);
    for (auto it = channels.begin(); it != channels.end();) {
        if ((*it)->id == channelId) {
            (*it)->input.reset();
            it = channels.erase(it);
        } else {
            ++it;
        }
    }
    routes.erase(std::remove_if(routes.begin(), routes.end(),
                               [channelId](const auto& route) { return route->channelId == channelId; }),
                  routes.end());
    pruneUnusedCaptures();
}

void AudioEngine::addChannel() {
    const juce::ScopedLock lock(stateLock);
    auto channel = std::make_unique<Channel>();
    channel->id = nextChannelId++;
    channel->name = "Channel " + juce::String(channel->id);
    channel->sourceKind = pluginrack::SourceKind::Hardware;
    channel->layout = pluginrack::ChannelLayout::Stereo;
    channel->inputChannels = {0, 1};
    channel->buffer.setSize(2, currentBufferSize);
    channel->buffer.clear();
    meters.observeChannel(channel->id, 0.0f);
    for (const auto& output : outputs) {
        auto route = std::make_unique<Route>();
        route->channelId = channel->id;
        route->outputId = output->id;
        const float g = output == outputs.front() ? 0.0f : -60.0f;
        route->sendGainDbL = g;
        route->sendGainDbR = g;
        routes.push_back(std::move(route));
    }
    channels.push_back(std::move(channel));
}

void AudioEngine::addOutput() {
    const juce::ScopedLock lock(stateLock);
    auto output = std::make_unique<Output>();
    output->id = nextOutputId++;
    if (output->id <= 5) output->name = "A" + juce::String(output->id);
    else if (output->id <= 8) output->name = "B" + juce::String(output->id - 5);
    else output->name = "Output " + juce::String(output->id);
    output->buffer.setSize(2, currentBufferSize);
    output->buffer.clear();
    meters.observeOutput(output->id, 0.0f);
    for (const auto& channel : channels) {
        auto route = std::make_unique<Route>();
        route->channelId = channel->id;
        route->outputId = output->id;
        route->sendGainDbL = -60.0f;
        route->sendGainDbR = -60.0f;
        routes.push_back(std::move(route));
    }
    outputs.push_back(std::move(output));
}

juce::String AudioEngine::removeOutput(std::uint32_t outputId) {
    // Find the output.
    AudioEngine::Output* out = nullptr;
    for (auto& o : outputs) {
        if (o->id == outputId) { out = o.get(); break; }
    }
    if (out == nullptr) {
        return "The output no longer exists.";
    }
    if (outputs.size() <= 1) {
        return "At least one output is required.";
    }

    const bool wasClock = (clockOutputId == outputId);
    if (wasClock && out->output != nullptr) {
        out->output->setRenderActive(false);
    }

    {
        const juce::ScopedLock sl(stateLock);
        if (out->output != nullptr) {
            out->output.reset();
        }
        routes.erase(std::remove_if(routes.begin(), routes.end(),
                                   [outputId](const auto& route) { return route->outputId == outputId; }),
                     routes.end());
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            if ((*it)->id == outputId) {
                it = outputs.erase(it);
                break;
            }
        }
        if (wasClock) {
            clockOutputId = 0;
            for (auto& o2 : outputs) {
                if (o2->output != nullptr) {
                    clockOutputId = o2->id;
                    o2->output->setRenderHook([this](int n) { this->render(n); });
                    break;
                }
            }
        }
    }
    return {};
}

juce::String AudioEngine::addPlugin(std::uint32_t channelId, const juce::File& file) {
    auto* channel = findChannel(channelId);
    if (channel == nullptr) {
        return "The channel no longer exists.";
    }
    if (channel->inserts.size() >= 4) {
        return "Each strip holds at most 4 inserts.";
    }

    auto insert = std::make_unique<InsertSlot>();
    insert->id = nextInsertId++;
    insert->pluginHost.prepareToPlay(currentSampleRate, currentBufferSize);
    const auto error = insert->pluginHost.loadPlugin(file);
    if (error.isNotEmpty()) {
        return error;
    }
    insert->pluginPath = file.getFullPathName();
    insert->pluginName = insert->pluginHost.getPluginName();
    insert->latencySamples = insert->pluginHost.getLatencySamples();

    const juce::ScopedLock lock(stateLock);
    if (channel->inserts.size() >= 4) {
        return "Each strip holds at most 4 inserts.";
    }
    channel->inserts.push_back(std::move(insert));
    return {};
}

void AudioEngine::removePlugin(std::uint32_t channelId, std::uint32_t insertId) {
    const juce::ScopedLock lock(stateLock);
    auto* channel = findChannel(channelId);
    if (channel == nullptr) {
        return;
    }
    auto& inserts = channel->inserts;
    inserts.erase(std::remove_if(inserts.begin(), inserts.end(), [insertId](const auto& insert) {
                      return insert->id == insertId;
                  }),
                  inserts.end());
}

void AudioEngine::setPluginBypassed(std::uint32_t channelId, std::uint32_t insertId, bool bypassed) {
    if (auto* insert = findInsert(channelId, insertId)) {
        insert->bypassed = bypassed;
        insert->pluginHost.refreshLatencyCache();
        insert->latencySamples = insert->pluginHost.getLatencySamples();
    }
}

juce::String AudioEngine::addOutputPlugin(std::uint32_t outputId, const juce::File& file) {
    auto* output = findOutput(outputId);
    if (output == nullptr) {
        return "The output no longer exists.";
    }
    if (output->inserts.size() >= 4) {
        return "Each strip holds at most 4 inserts.";
    }

    auto insert = std::make_unique<InsertSlot>();
    insert->id = nextInsertId++;
    insert->pluginHost.prepareToPlay(currentSampleRate, currentBufferSize);
    const auto error = insert->pluginHost.loadPlugin(file);
    if (error.isNotEmpty()) {
        return error;
    }
    insert->pluginPath = file.getFullPathName();
    insert->pluginName = insert->pluginHost.getPluginName();
    insert->latencySamples = insert->pluginHost.getLatencySamples();

    const juce::ScopedLock lock(stateLock);
    if (output->inserts.size() >= 4) {
        return "Each strip holds at most 4 inserts.";
    }
    output->inserts.push_back(std::move(insert));
    return {};
}

void AudioEngine::removeOutputPlugin(std::uint32_t outputId, std::uint32_t insertId) {
    const juce::ScopedLock lock(stateLock);
    auto* output = findOutput(outputId);
    if (output == nullptr) {
        return;
    }
    auto& inserts = output->inserts;
    inserts.erase(std::remove_if(inserts.begin(), inserts.end(), [insertId](const auto& insert) {
                      return insert->id == insertId;
                  }),
                  inserts.end());
}

void AudioEngine::setOutputPluginBypassed(std::uint32_t outputId, std::uint32_t insertId, bool bypassed) {
    if (auto* insert = findOutputInsert(outputId, insertId)) {
        insert->bypassed = bypassed;
        insert->pluginHost.refreshLatencyCache();
        insert->latencySamples = insert->pluginHost.getLatencySamples();
    }
}

void AudioEngine::setChannelGain(std::uint32_t channelId, float gainDb) {
    if (auto* channel = findChannel(channelId)) {
        channel->gainDb = gainDb;
    }
}

void AudioEngine::setChannelTrim(std::uint32_t channelId, float trimDb) {
    if (auto* channel = findChannel(channelId)) {
        channel->trimDb = trimDb;
    }
}

void AudioEngine::setChannelPan(std::uint32_t channelId, float pan) {
    if (auto* channel = findChannel(channelId)) {
        channel->pan = pan;
    }
}

void AudioEngine::setChannelMuted(std::uint32_t channelId, bool muted) {
    if (auto* channel = findChannel(channelId)) {
        channel->muted = muted;
    }
}

void AudioEngine::setChannelSoloed(std::uint32_t channelId, bool soloed) {
    if (auto* channel = findChannel(channelId)) {
        channel->soloed = soloed;
    }
}

void AudioEngine::setChannelName(std::uint32_t channelId, const juce::String& name) {
    const juce::ScopedLock lock(stateLock);
    if (auto* channel = findChannel(channelId)) {
        channel->name = name;
    }
}

void AudioEngine::setChannelLayout(std::uint32_t channelId, pluginrack::ChannelLayout layout) {
    juce::String deviceName;
    {
        const juce::ScopedLock lock(stateLock);
        auto* channel = findChannel(channelId);
        if (channel == nullptr) {
            return;
        }
        channel->layout = layout;
        deviceName = channel->inputDeviceName;
    }
    // Re-assign the device so the shared capture is re-opened (or widened)
    // for the new layout; silent channels keep nothing to reopen.
    if (deviceName.isNotEmpty() && deviceName != "None - silent") {
        setChannelInputDevice(channelId, deviceName);
    }
}

void AudioEngine::setChannelInputChannels(std::uint32_t channelId, const std::vector<int>& inputChannels) {
    juce::String deviceName;
    {
        const juce::ScopedLock lock(stateLock);
        auto* channel = findChannel(channelId);
        if (channel == nullptr) {
            return;
        }
        channel->inputChannels = inputChannels;
        deviceName = channel->inputDeviceName;
    }
    if (deviceName.isNotEmpty() && deviceName != "None - silent") {
        setChannelInputDevice(channelId, deviceName);
    }
}

void AudioEngine::setChannelMapping(std::uint32_t channelId, pluginrack::ChannelLayout layout, const std::vector<int>& inputChannels) {
    juce::String deviceName;
    {
        const juce::ScopedLock lock(stateLock);
        auto* channel = findChannel(channelId);
        if (channel == nullptr) return;
        channel->layout = layout;
        channel->inputChannels = inputChannels;
        deviceName = channel->inputDeviceName;
    }
    if (deviceName.isNotEmpty() && deviceName != "None - silent") {
        setChannelInputDevice(channelId, deviceName);
    }
}

void AudioEngine::setSendGain(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDbL = gainDb;
            route->sendGainDbR = gainDb;
            return;
        }
    }
}

void AudioEngine::setSendGainL(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDbL = gainDb;
            return;
        }
    }
}

void AudioEngine::setSendGainR(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDbR = gainDb;
            return;
        }
    }
}

void AudioEngine::setOutputGain(std::uint32_t outputId, float gainDb) {
    for (auto& output : outputs) {
        if (output->id == outputId) { output->gainDb = gainDb; return; }
    }
}

void AudioEngine::setOutputMuted(std::uint32_t outputId, bool muted) {
    for (auto& output : outputs) {
        if (output->id == outputId) { output->muted = muted; return; }
    }
}

void AudioEngine::setOutputName(std::uint32_t outputId, const juce::String& name) {
    const juce::ScopedLock lock(stateLock);
    for (auto& output : outputs) {
        if (output->id == outputId) { output->name = name; return; }
    }
}

void AudioEngine::setOutputLayout(std::uint32_t outputId, pluginrack::ChannelLayout layout) {
    const juce::ScopedLock lock(stateLock);
    for (auto& output : outputs) {
        if (output->id == outputId) { output->layout = layout; return; }
    }
}

void AudioEngine::setOutputFlipped(std::uint32_t outputId, bool flipped) {
    for (auto& output : outputs) {
        if (output->id == outputId) { output->flipped = flipped; return; }
    }
}

float AudioEngine::getSendGain(std::uint32_t channelId, std::uint32_t outputId) const {
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->sendGainDbL.get();
        }
    }
    return -60.0f;
}

float AudioEngine::getSendGainL(std::uint32_t channelId, std::uint32_t outputId) const {
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->sendGainDbL.get();
        }
    }
    return -60.0f;
}

float AudioEngine::getSendGainR(std::uint32_t channelId, std::uint32_t outputId) const {
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->sendGainDbR.get();
        }
    }
    return -60.0f;
}

const std::vector<std::unique_ptr<AudioEngine::Channel>>& AudioEngine::getChannels() const {
    return channels;
}

const std::vector<std::unique_ptr<AudioEngine::Output>>& AudioEngine::getOutputs() const {
    return outputs;
}

const std::vector<std::unique_ptr<AudioEngine::Route>>& AudioEngine::getRoutes() const {
    return routes;
}

pluginrack::Scene AudioEngine::makeScene() const {
    const juce::ScopedLock lock(stateLock);
    pluginrack::Scene scene;
    scene.name = "Default";
    scene.sampleRate = currentSampleRate;
    scene.bufferFrames = currentBufferSize;
    for (const auto& channel : channels) {
        pluginrack::Channel savedChannel;
        savedChannel.id = channel->id;
        savedChannel.name = channel->name.toStdString();
        savedChannel.sourceKind = channel->sourceKind;
        savedChannel.sourceDeviceId = channel->sourceDeviceId;
        savedChannel.layout = channel->layout;
        savedChannel.inputChannels = channel->inputChannels;
        savedChannel.gainDb = channel->gainDb.get();
        savedChannel.pan = channel->pan.get();
        savedChannel.muted = channel->muted.get();
        savedChannel.soloed = channel->soloed.get();
        for (const auto& insert : channel->inserts) {
            savedChannel.inserts.push_back({insert->id, insert->pluginPath.toStdString(),
                                            insert->pluginName.toStdString(), insert->bypassed.get()});
        }
        scene.channels.push_back(std::move(savedChannel));
    }
    for (const auto& output : outputs) {
        pluginrack::Output savedOutput;
        savedOutput.id = output->id;
        savedOutput.name = output->name.toStdString();
        savedOutput.layout = output->layout;
        savedOutput.flipped = output->flipped.get();
        for (const auto& insert : output->inserts) {
            savedOutput.inserts.push_back({insert->id, insert->pluginPath.toStdString(),
                                           insert->pluginName.toStdString(), insert->bypassed.get()});
        }
        scene.outputs.push_back(std::move(savedOutput));
    }
    for (const auto& route : routes) {
        scene.routes.push_back({route->channelId, route->outputId, route->sendGainDbL.get()});
    }
    return scene;
}

juce::String AudioEngine::restoreScene(const pluginrack::Scene& scene) {
    double sampleRate;
    int bufferSize;
    std::map<std::uint32_t, juce::String> savedInputDevices;
    std::map<std::uint32_t, juce::String> savedOutputDevices;
    {
        const juce::ScopedLock lock(stateLock);
        sampleRate = currentSampleRate;
        bufferSize = currentBufferSize;
        for (const auto& c : channels) savedInputDevices[c->id] = c->inputDeviceName;
        for (const auto& o : outputs) savedOutputDevices[o->id] = o->outputDeviceName;
    }

    std::vector<std::unique_ptr<Channel>> restoredChannels;
    std::vector<std::unique_ptr<Output>> restoredOutputs;
    std::vector<std::unique_ptr<Route>> restoredRoutes;
    pluginrack::MeterBank restoredMeters;
    juce::String loadErrors;

    for (const auto& savedChannel : scene.channels) {
        auto channel = std::make_unique<Channel>();
        channel->id = savedChannel.id;
        channel->name = savedChannel.name;
        channel->gainDb = savedChannel.gainDb;
        channel->pan = savedChannel.pan;
        channel->muted = savedChannel.muted;
        channel->soloed = savedChannel.soloed;
        channel->sourceKind = savedChannel.sourceKind;
        channel->sourceDeviceId = savedChannel.sourceDeviceId;
        channel->layout = savedChannel.layout;
        channel->inputChannels = savedChannel.inputChannels;
        if (channel->inputChannels.empty()) {
            channel->inputChannels = channel->layout == pluginrack::ChannelLayout::Mono
                                        ? std::vector<int>{0}
                                        : std::vector<int>{0, 1};
        }
        channel->buffer.setSize(2, bufferSize);
        channel->buffer.clear();
        for (const auto& savedInsert : savedChannel.inserts) {
            auto insert = std::make_unique<InsertSlot>();
            insert->id = savedInsert.id;
            insert->pluginPath = savedInsert.pluginPath;
            insert->pluginName = savedInsert.pluginName;
            insert->bypassed = savedInsert.bypassed;
            insert->pluginHost.prepareToPlay(sampleRate, bufferSize);
            const auto error = insert->pluginHost.loadPlugin(juce::File(insert->pluginPath));
            if (error.isNotEmpty()) {
                loadErrors += channel->name + ": " + error + "\n";
            }
            insert->latencySamples = insert->pluginHost.getLatencySamples();
            nextInsertId = juce::jmax(nextInsertId, insert->id + 1);
            channel->inserts.push_back(std::move(insert));
        }
        nextChannelId = juce::jmax(nextChannelId, channel->id + 1);
        restoredMeters.observeChannel(channel->id, 0.0f);
        restoredChannels.push_back(std::move(channel));
    }
    for (const auto& savedOutput : scene.outputs) {
        auto output = std::make_unique<Output>();
        output->id = savedOutput.id;
        output->name = savedOutput.name;
        output->layout = savedOutput.layout;
        output->flipped = savedOutput.flipped;
        output->buffer.setSize(2, bufferSize);
        output->buffer.clear();
        for (const auto& savedInsert : savedOutput.inserts) {
            auto insert = std::make_unique<InsertSlot>();
            insert->id = savedInsert.id;
            insert->pluginPath = savedInsert.pluginPath;
            insert->pluginName = savedInsert.pluginName;
            insert->bypassed = savedInsert.bypassed;
            insert->pluginHost.prepareToPlay(sampleRate, bufferSize);
            const auto error = insert->pluginHost.loadPlugin(juce::File(insert->pluginPath));
            if (error.isNotEmpty()) {
                loadErrors += output->name + ": " + error + "\n";
            }
            insert->latencySamples = insert->pluginHost.getLatencySamples();
            nextInsertId = juce::jmax(nextInsertId, insert->id + 1);
            output->inserts.push_back(std::move(insert));
        }
        nextOutputId = juce::jmax(nextOutputId, output->id + 1);
        restoredMeters.observeOutput(output->id, 0.0f);
        restoredOutputs.push_back(std::move(output));
    }
    for (const auto& savedRoute : scene.routes) {
        auto route = std::make_unique<Route>();
        route->channelId = savedRoute.channelId;
        route->outputId = savedRoute.outputId;
        route->sendGainDbL = savedRoute.sendGainDb;
        route->sendGainDbR = savedRoute.sendGainDb;
        restoredRoutes.push_back(std::move(route));
    }

    {
        const juce::ScopedLock lock(stateLock);
        channels = std::move(restoredChannels);
        outputs = std::move(restoredOutputs);
        routes = std::move(restoredRoutes);
        meters = std::move(restoredMeters);
        // Old channels dropped above may have left captures nobody uses.
        pruneUnusedCaptures();
    }

    // Re-apply device assignments where possible: the device stored in the
    // scene wins; channels from scenes saved before sourceDeviceId was
    // persisted keep whatever device was live for the same id.
    std::map<std::uint32_t, juce::String> desiredInputDevices;
    for (const auto& savedChannel : scene.channels) {
        const juce::String sceneDevice(savedChannel.sourceDeviceId);
        if (sceneDevice.isNotEmpty()) {
            desiredInputDevices[savedChannel.id] = sceneDevice;
            continue;
        }
        const auto live = savedInputDevices.find(savedChannel.id);
        if (live != savedInputDevices.end() && live->second.isNotEmpty()) {
            desiredInputDevices[savedChannel.id] = live->second;
        }
    }
    for (const auto& [id, name] : desiredInputDevices) {
        const auto e = setChannelInputDevice(id, name);
        if (e.isNotEmpty()) loadErrors += e + "\n";
    }
    for (const auto& [id, name] : savedOutputDevices) {
        if (name.isNotEmpty()) {
            const auto e = setOutputDevice(id, name);
            if (e.isNotEmpty()) loadErrors += e + "\n";
        }
    }

    return loadErrors;
}

juce::String AudioEngine::setChannelInputDevice(std::uint32_t channelId, const juce::String& deviceName) {
    auto* channel = findChannel(channelId);
    if (channel == nullptr) {
        return "The channel no longer exists.";
    }

    if (deviceName == "None - silent" || deviceName.isEmpty()) {
        const juce::ScopedLock sl(stateLock);
        channel->input.reset();
        channel->inputDeviceName = "None - silent";
        channel->sourceDeviceId.clear();
        channel->inputError.clear();
        pruneUnusedCaptures();
        return {};
    }

    // The shared capture for this device must cover the highest physical index
    // used by any channel on it, including this channel's new mapping.
    int needed = 2;
    {
        const juce::ScopedLock sl(stateLock);
        for (const auto& c : channels) {
            if (c.get() == channel || c->inputDeviceName == deviceName) {
                for (const int index : c->inputChannels) {
                    needed = juce::jmax(needed, index + 1);
                }
            }
        }
    }

    std::shared_ptr<InputCapture> reuse;
    {
        const juce::ScopedLock sl(stateLock);
        const auto it = inputCaptures_.find(deviceName);
        if (it != inputCaptures_.end() && it->second.requestedChannels >= needed
            && (requestedBufferSize_ == 0 || it->second.capture->getDeviceBufferSize() == requestedBufferSize_)) {
            reuse = it->second.capture;
        }
    }

    if (reuse != nullptr) {
        // One device stays open for every logical channel mapped into it.
        const juce::ScopedLock sl(stateLock);
        channel->input = reuse;
        channel->inputDeviceName = deviceName;
        channel->sourceDeviceId = deviceName.toStdString();
        channel->inputError = reuse->getInputError();
        pruneUnusedCaptures();
        return {};
    }

    // No usable capture yet (or it is too narrow): open one sized for all
    // channels on this device. On failure every channel keeps its previous
    // capture untouched.
    auto capture = std::make_shared<InputCapture>();
    juce::String err;
    const auto openErr = capture->open(deviceName == "System Default" ? juce::String() : deviceName,
                                       currentSampleRate, needed, err, requestedBufferSize_);
    if (openErr.isNotEmpty()) {
        const juce::ScopedLock sl(stateLock);
        channel->inputError = openErr;
        return openErr;
    }

    {
        const juce::ScopedLock sl(stateLock);
        for (auto& c : channels) {
            if (c.get() == channel || c->inputDeviceName == deviceName) {
                c->input = capture;
                c->inputDeviceName = deviceName;
                c->sourceDeviceId = deviceName.toStdString();
                c->inputError = capture->getInputError();
            }
        }
        inputCaptures_[deviceName] = SharedInputCapture{capture, needed};
        pruneUnusedCaptures();
    }
    return {};
}

void AudioEngine::prepareInputs() {
    std::vector<std::pair<std::uint32_t, juce::String>> wanted;
    {
        const juce::ScopedLock lock(stateLock);
        for (const auto& channel : channels) {
            if (channel->inputDeviceName.isNotEmpty()
                && channel->inputDeviceName != "None - silent") {
                wanted.emplace_back(channel->id, channel->inputDeviceName);
            }
        }
    }
    // setChannelInputDevice reuses an existing capture when the requested
    // width still covers the mappings and reopens wider when they grew.
    for (const auto& [id, name] : wanted) {
        setChannelInputDevice(id, name);
    }
}

void AudioEngine::pruneUnusedCaptures() {
    for (auto it = inputCaptures_.begin(); it != inputCaptures_.end();) {
        // Only the map itself holds the capture: no channel uses it anymore.
        if (it->second.capture == nullptr || it->second.capture.use_count() == 1) {
            it = inputCaptures_.erase(it);  // destructor closes the device
        } else {
            ++it;
        }
    }
}

juce::String AudioEngine::setOutputDevice(std::uint32_t outputId, const juce::String& deviceName) {
    AudioEngine::Output* out = nullptr;
    for (auto& o : outputs) {
        if (o->id == outputId) { out = o.get(); break; }
    }
    if (out == nullptr) {
        return "The output no longer exists.";
    }

    const bool isClock = (clockOutputId == outputId) || (clockOutputId == 0);

    if (deviceName == "None - silent" || deviceName.isEmpty()) {
        const bool wasClock = (clockOutputId == outputId);
        {
            const juce::ScopedLock sl(stateLock);
            out->output.reset();
            out->outputDeviceName.clear();
            out->outputError.clear();
            if (wasClock) {
                clockOutputId = 0;
                for (auto& o2 : outputs) {
                    if (o2->output != nullptr) {
                        clockOutputId = o2->id;
                        o2->output->setRenderHook([this](int n) { this->render(n); });
                        break;
                    }
                }
            }
        }
        return {};
    }

    const juce::String prevName = out->outputDeviceName;
    auto prevOutput = std::move(out->output);
    if (prevOutput != nullptr && isClock) {
        // Stop the old clock before a second render hook can run.
        prevOutput->setRenderActive(false);
    }

    auto newPlayback = std::make_unique<OutputPlayback>();
    if (isClock) {
        newPlayback->setRenderHook([this](int n) { this->render(n); });
    }

    juce::String err;
    const double rateForValidation = isClock ? 0.0 : currentSampleRate;
    const auto openErr = newPlayback->open(deviceName == "System Default" ? juce::String() : deviceName,
                                          rateForValidation, err, requestedBufferSize_);
    if (openErr.isNotEmpty()) {
        const juce::ScopedLock sl(stateLock);
        out->output = std::move(prevOutput);
        if (out->output != nullptr) out->output->setRenderActive(true);
        out->outputError = openErr;
        return openErr;
    }

    if (isClock) {
        const double newRate = newPlayback->getSampleRate();
        int newBuf = newPlayback->getBufferSize();
        if (newBuf <= 0) newBuf = 256;
        {
            const juce::ScopedLock sl(stateLock);
            currentSampleRate = newRate;
            clockOutputId = outputId;
        }
        prepareToPlay(newBuf);
    } else {
        // ensure clock id remains; if no clock, maybe set? handled by isClock true when none
    }

    {
        const juce::ScopedLock sl(stateLock);
        out->output = std::move(newPlayback);
        out->outputDeviceName = deviceName;
        out->outputError.clear();
    }
    return {};
}

juce::String AudioEngine::setDeviceBufferSize(int size) {
    requestedBufferSize_ = size;
    juce::String firstError;
    for (auto& channel : channels) {
        if (channel->inputDeviceName.isNotEmpty() &&
            channel->inputDeviceName != "None - silent") {
            const auto e = setChannelInputDevice(channel->id, channel->inputDeviceName);
            if (firstError.isEmpty() && e.isNotEmpty()) firstError = e;
        }
    }
    for (auto& output : outputs) {
        if (output->outputDeviceName.isNotEmpty() &&
            output->outputDeviceName != "None - silent") {
            const auto e = setOutputDevice(output->id, output->outputDeviceName);
            if (firstError.isEmpty() && e.isNotEmpty()) firstError = e;
        }
    }
    return firstError;
}

juce::String AudioEngine::getClockDescription() const {
    if (clockOutputId == 0) {
        return "No output device - click a master strip";
    }
    for (const auto& o : outputs) {
        if (o->id == clockOutputId) {
            const auto device = o->outputDeviceName.isEmpty() ? juce::String("System Default")
                                                              : o->outputDeviceName;
            const auto khzValue = currentSampleRate / 1000.0;
            const auto khz = std::abs(khzValue - std::round(khzValue)) < 0.001
                                 ? juce::String((int)std::round(khzValue))
                                 : juce::String(khzValue, 1);
            return o->name + "  -  " + device + "  -  " + khz + " kHz  -  "
                   + juce::String(currentBufferSize) + " smp";
        }
    }
    return "No output device - click a master strip";
}

juce::StringArray AudioEngine::getAvailableInputDevices() const {
    return DeviceEnumeration::inputs();
}

juce::StringArray AudioEngine::getAvailableOutputDevices() const {
    return DeviceEnumeration::outputs();
}

AudioEngine::Channel* AudioEngine::findChannel(std::uint32_t channelId) {
    for (auto& channel : channels) {
        if (channel->id == channelId) {
            return channel.get();
        }
    }
    return nullptr;
}

AudioEngine::InsertSlot* AudioEngine::findInsert(std::uint32_t channelId, std::uint32_t insertId) {
    auto* channel = findChannel(channelId);
    if (channel == nullptr) {
        return nullptr;
    }
    for (auto& insert : channel->inserts) {
        if (insert->id == insertId) {
            return insert.get();
        }
    }
    return nullptr;
}

AudioEngine::Output* AudioEngine::findOutput(std::uint32_t outputId) {
    for (auto& output : outputs) {
        if (output->id == outputId) {
            return output.get();
        }
    }
    return nullptr;
}

AudioEngine::InsertSlot* AudioEngine::findOutputInsert(std::uint32_t outputId, std::uint32_t insertId) {
    auto* output = findOutput(outputId);
    if (output == nullptr) {
        return nullptr;
    }
    for (auto& insert : output->inserts) {
        if (insert->id == insertId) {
            return insert.get();
        }
    }
    return nullptr;
}

AudioEngine::LatencyReport AudioEngine::getLatencyReport() const {
    LatencyReport report;
    const juce::ScopedTryLock lock(stateLock);
    if (!lock.isLocked()) {
        report.valid = false;
        return report;
    }

    const Channel* inputChannel = nullptr;
    for (const auto& channel : channels) {
        if (channel->input != nullptr) {
            inputChannel = channel.get();
            break;
        }
    }

    int inputDeviceSamples = 0;
    int inputBufferSize = 0;
    double inputOcc = 0.0;
    if (inputChannel != nullptr && inputChannel->input != nullptr) {
        inputDeviceSamples = inputChannel->input->getLatencySamples();
        inputBufferSize = inputChannel->input->getDeviceBufferSize();
        if (inputBufferSize <= 0) inputBufferSize = currentBufferSize;
        inputOcc = inputChannel->input->getOccupancy();
    }

    int pluginSamples = 0;
    int maxInsertSamples = 0;
    std::uint32_t maxInsertId = 0;
    juce::String maxInsertName;
    if (inputChannel != nullptr && inputChannel->input != nullptr) {
        for (const auto& insert : inputChannel->inserts) {
            pluginSamples += insert->latencySamples;
            if (insert->latencySamples > maxInsertSamples) {
                maxInsertSamples = insert->latencySamples;
                maxInsertId = insert->id;
                maxInsertName = insert->pluginName;
            }
        }
    }

    int outputDeviceSamples = 0;
    int outputBufferSize = 0;
    double outputOcc = 0.0;
    bool valid = false;
    if (clockOutputId != 0) {
        for (const auto& output : outputs) {
            if (output->id == clockOutputId && output->output != nullptr) {
                outputDeviceSamples = output->output->getLatencySamples();
                outputBufferSize = output->output->getBufferSize();
                if (outputBufferSize <= 0) outputBufferSize = currentBufferSize;
                outputOcc = output->output->getOccupancy();
                for (const auto& insert : output->inserts) {
                    pluginSamples += insert->latencySamples;
                    if (insert->latencySamples > maxInsertSamples) {
                        maxInsertSamples = insert->latencySamples;
                        maxInsertId = insert->id;
                        maxInsertName = insert->pluginName;
                    }
                }
                valid = true;
                break;
            }
        }
    }

    const double sampleRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    const double fifoMs = (inputOcc + outputOcc) * 0.5 * currentBufferSize / sampleRate * 1000.0;
    // getLatencySamples() already reports device latency + one device
    // buffer (see DeviceStream), so the device terms carry that combined
    // structural delay; the buffer sizes are reported separately below.
    report.inputDeviceMs = inputDeviceSamples / sampleRate * 1000.0;
    report.outputDeviceMs = outputDeviceSamples / sampleRate * 1000.0;
    report.pluginMs = pluginSamples / sampleRate * 1000.0;
    report.maxInsertId = maxInsertId;
    report.maxInsertName = maxInsertName;
    report.maxInsertMs = maxInsertSamples / sampleRate * 1000.0;
    report.fifoMs = fifoMs;
    report.totalMs = report.inputDeviceMs + report.pluginMs + report.outputDeviceMs + report.fifoMs;
    report.inputBufferSize = inputBufferSize;
    report.outputBufferSize = outputBufferSize;
    report.sampleRate = sampleRate;
    // Aggregate the FIFO counters directly: getFifoDiagnostics() takes its
    // own try-lock on stateLock, which is already held here, so calling it
    // would always fall through to zeros.
    report.underruns = 0;
    report.overruns = 0;
    for (const auto& entry : inputCaptures_) {
        if (entry.second.capture != nullptr) {
            report.overruns += entry.second.capture->getInputOverruns();
        }
    }
    for (const auto& output : outputs) {
        if (output->output != nullptr) {
            report.underruns += output->output->getOutputUnderruns();
        }
    }
    report.valid = valid;
    report.approximate = true;
    report.measuredValid = latencyMeasuredValid_.load(std::memory_order_relaxed);
    report.measuredMs = report.measuredValid ? getMeasuredLatencyMs() : 0.0;
    report.measureFailed = latencyMeasureFailed_.load(std::memory_order_relaxed);
    report.measuredAttempted = report.measuredValid || report.measureFailed
        || latencyMeasureActive_.load(std::memory_order_relaxed);
    return report;
}

AudioEngine::FifoDiagnostics AudioEngine::getFifoDiagnostics() const {
    FifoDiagnostics diagnostics;
    const juce::ScopedTryLock lock(stateLock);
    if (!lock.isLocked()) {
        return diagnostics;
    }

    for (const auto& entry : inputCaptures_) {
        const auto& capture = entry.second.capture;
        if (capture != nullptr) {
            diagnostics.inputOverruns += capture->getInputOverruns();
            diagnostics.inputDroppedFrames += capture->getInputDroppedFrames();
        }
    }

    for (const auto& output : outputs) {
        if (output->output != nullptr) {
            diagnostics.outputUnderruns += output->output->getOutputUnderruns();
            diagnostics.outputDroppedFrames += output->output->getOutputDroppedFrames();
        }
    }

    return diagnostics;
}

void AudioEngine::startLatencyMeasurement() {
    const juce::ScopedLock lock(stateLock);
    latencyMeasureActive_.store(true, std::memory_order_relaxed);
    latencyMeasureEmitted_.store(false, std::memory_order_relaxed);
    latencyMeasureOutputSample_.store(0, std::memory_order_relaxed);
    latencyMeasureBlocks_.store(0, std::memory_order_relaxed);
    latencyMeasuredValid_.store(false, std::memory_order_relaxed);
    latencyMeasuredSamples_.store(0, std::memory_order_relaxed);
    latencyMeasureFailed_.store(false, std::memory_order_relaxed);
    latencyMeasureStartMs_.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    outputSampleCounter_ = 0;
    for (auto& entry : inputCaptures_) {
        if (entry.second.capture) entry.second.capture->setLatencyMeasurementActive(true);
    }
}

void AudioEngine::cancelLatencyMeasurementImpl() {
    latencyMeasureActive_.store(false, std::memory_order_relaxed);
    for (auto& entry : inputCaptures_) {
        if (entry.second.capture) entry.second.capture->setLatencyMeasurementActive(false);
    }
}

void AudioEngine::cancelLatencyMeasurement() {
    const juce::ScopedLock lock(stateLock);
    cancelLatencyMeasurementImpl();
}

double AudioEngine::getMeasuredLatencyMs() const {
    const double sr = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    return latencyMeasuredSamples_.load(std::memory_order_relaxed) / sr * 1000.0;
}

AudioEngine::HighestLatencyInsert AudioEngine::getHighestLatencyInsert(std::uint32_t channelId) const {
    HighestLatencyInsert result;
    result.channelId = channelId;
    const juce::ScopedTryLock lock(stateLock);
    if (!lock.isLocked()) return result;
    const Channel* ch = nullptr;
    for (const auto& c : channels) if (c->id == channelId) { ch = c.get(); break; }
    if (ch == nullptr) return result;
    int maxLat = 0;
    for (const auto& ins : ch->inserts) {
        if (ins->latencySamples > maxLat) {
            maxLat = ins->latencySamples;
            result.insertId = ins->id;
            result.name = ins->pluginName;
            result.latencySamples = ins->latencySamples;
        }
    }
    return result;
}

bool AudioEngine::openFullDuplexIfPossible(const juce::String& inputDevice, const juce::String& outputDevice) {
    fullDuplex_ = false;
    if (inputDevice.isEmpty() || outputDevice.isEmpty()) return false;
    if (inputDevice != outputDevice) return false;
    // Same device: use existing separate streams (device provides both directions).
    // Single render is guaranteed by clockOutputId (only the clock output renders).
    fullDuplex_ = true;
    return true;
}

float AudioEngine::decibelsToGain(float gainDb) {
    return gainDb <= -60.0f ? 0.0f : std::pow(10.0f, gainDb / 20.0f);
}
