#include "AudioEngine.h"

#include <algorithm>
#include <cmath>

AudioEngine::AudioEngine() {
    addOutput();
    addChannel();
}

void AudioEngine::prepareToPlay(int samplesPerBlockExpected, double sampleRate) {
    const juce::ScopedLock lock(stateLock);
    currentSampleRate = sampleRate;
    currentBufferSize = juce::jmax(1, samplesPerBlockExpected);
    for (auto& channel : channels) {
        channel->buffer.setSize(2, currentBufferSize);
        channel->buffer.clear();
        for (auto& insert : channel->inserts) {
            insert->pluginHost.prepareToPlay(sampleRate, currentBufferSize);
        }
        meters.observeChannel(channel->id, 0.0f);
    }
    for (auto& output : outputs) {
        output->buffer.setSize(2, currentBufferSize);
        output->buffer.clear();
        meters.observeOutput(output->id, 0.0f);
    }
}

void AudioEngine::processBlock(const juce::AudioSourceChannelInfo& bufferToFill) {
    auto* deviceBuffer = bufferToFill.buffer;
    if (deviceBuffer == nullptr || bufferToFill.numSamples <= 0) {
        return;
    }

    const juce::ScopedTryLock lock(stateLock);
    if (!lock.isLocked()) {
        deviceBuffer->clear(bufferToFill.startSample, bufferToFill.numSamples);
        return;
    }

    const int frames = juce::jmin(bufferToFill.numSamples, currentBufferSize);
    const int deviceChannels = deviceBuffer->getNumChannels();
    for (auto& channel : channels) {
        channel->buffer.clear(0, frames);
        for (int destination = 0; destination < 2 && deviceChannels > 0; ++destination) {
            const int source = juce::jmin(destination, deviceChannels - 1);
            channel->buffer.copyFrom(destination, 0, *deviceBuffer, source,
                                     bufferToFill.startSample, frames);
        }
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
        const float gain = silent ? 0.0f : decibelsToGain(channel->gainDb.get());
        const float theta = (channel->pan.get() + 1.0f) * juce::MathConstants<float>::pi / 4.0f;
        channel->buffer.applyGain(0, 0, frames, gain * std::cos(theta));
        channel->buffer.applyGain(1, 0, frames, gain * std::sin(theta));

        const float peak = juce::jmax(channel->buffer.getMagnitude(0, 0, frames),
                                      channel->buffer.getMagnitude(1, 0, frames));
        meters.observeChannel(channel->id, peak);
        channel->peak = meters.channelPeak(channel->id);
    }

    for (auto& output : outputs) {
        output->buffer.clear(0, frames);
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

        const float sendGain = decibelsToGain(route->sendGainDb.get());
        if (sendGain == 0.0f) {
            continue;
        }
        for (int side = 0; side < 2; ++side) {
            (*outputIt)->buffer.addFrom(side, 0, (*channelIt)->buffer, side, 0, frames, sendGain);
        }
    }

    for (auto& output : outputs) {
        const float peak = juce::jmax(output->buffer.getMagnitude(0, 0, frames),
                                      output->buffer.getMagnitude(1, 0, frames));
        meters.observeOutput(output->id, peak);
        output->peak = meters.outputPeak(output->id);
    }

    deviceBuffer->clear(bufferToFill.startSample, bufferToFill.numSamples);
    for (const auto& output : outputs) {
        for (int side = 0; side < juce::jmin(2, deviceChannels); ++side) {
            deviceBuffer->addFrom(side, bufferToFill.startSample, output->buffer, side, 0, frames);
        }
    }
}

void AudioEngine::releaseResources() {
    const juce::ScopedLock lock(stateLock);
    for (auto& channel : channels) {
        for (auto& insert : channel->inserts) {
            insert->pluginHost.releaseResources();
        }
    }
}

void AudioEngine::addChannel() {
    const juce::ScopedLock lock(stateLock);
    auto channel = std::make_unique<Channel>();
    channel->id = nextChannelId++;
    channel->name = "Channel " + juce::String(channel->id);
    channel->buffer.setSize(2, currentBufferSize);
    channel->buffer.clear();
    meters.observeChannel(channel->id, 0.0f);
    for (const auto& output : outputs) {
        auto route = std::make_unique<Route>();
        route->channelId = channel->id;
        route->outputId = output->id;
        route->sendGainDb = output == outputs.front() ? 0.0f : -60.0f;
        routes.push_back(std::move(route));
    }
    channels.push_back(std::move(channel));
}

void AudioEngine::addOutput() {
    const juce::ScopedLock lock(stateLock);
    auto output = std::make_unique<Output>();
    output->id = nextOutputId++;
    output->name = output->id == 1 ? "Main" : "Output " + juce::String(output->id);
    output->buffer.setSize(2, currentBufferSize);
    output->buffer.clear();
    meters.observeOutput(output->id, 0.0f);
    for (const auto& channel : channels) {
        auto route = std::make_unique<Route>();
        route->channelId = channel->id;
        route->outputId = output->id;
        route->sendGainDb = -60.0f;
        routes.push_back(std::move(route));
    }
    outputs.push_back(std::move(output));
}

juce::String AudioEngine::addPlugin(std::uint32_t channelId, const juce::File& file) {
    auto* channel = findChannel(channelId);
    if (channel == nullptr) {
        return "The channel no longer exists.";
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

    const juce::ScopedLock lock(stateLock);
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
    }
}

void AudioEngine::setChannelGain(std::uint32_t channelId, float gainDb) {
    if (auto* channel = findChannel(channelId)) {
        channel->gainDb = gainDb;
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

void AudioEngine::setSendGain(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDb = gainDb;
            return;
        }
    }
}

float AudioEngine::getSendGain(std::uint32_t channelId, std::uint32_t outputId) const {
    for (const auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            return route->sendGainDb.get();
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
        savedChannel.layout = pluginrack::ChannelLayout::Stereo;
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
        scene.outputs.push_back({output->id, output->name.toStdString()});
    }
    for (const auto& route : routes) {
        scene.routes.push_back({route->channelId, route->outputId, route->sendGainDb.get()});
    }
    return scene;
}

juce::String AudioEngine::restoreScene(const pluginrack::Scene& scene) {
    double sampleRate;
    int bufferSize;
    {
        const juce::ScopedLock lock(stateLock);
        sampleRate = currentSampleRate;
        bufferSize = currentBufferSize;
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
        output->buffer.setSize(2, bufferSize);
        output->buffer.clear();
        nextOutputId = juce::jmax(nextOutputId, output->id + 1);
        restoredMeters.observeOutput(output->id, 0.0f);
        restoredOutputs.push_back(std::move(output));
    }
    for (const auto& savedRoute : scene.routes) {
        auto route = std::make_unique<Route>();
        route->channelId = savedRoute.channelId;
        route->outputId = savedRoute.outputId;
        route->sendGainDb = savedRoute.sendGainDb;
        restoredRoutes.push_back(std::move(route));
    }

    const juce::ScopedLock lock(stateLock);
    channels = std::move(restoredChannels);
    outputs = std::move(restoredOutputs);
    routes = std::move(restoredRoutes);
    meters = std::move(restoredMeters);
    return loadErrors;
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

float AudioEngine::decibelsToGain(float gainDb) {
    return gainDb <= -60.0f ? 0.0f : std::pow(10.0f, gainDb / 20.0f);
}
