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
        for (auto& insert : output->inserts) {
            insert->pluginHost.prepareToPlay(sampleRate, currentBufferSize);
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

    // ensure buffers
    for (auto& channel : channels) {
        if (channel->buffer.getNumSamples() < frames) {
            channel->buffer.setSize(2, frames, true, true, true);
        }
        channel->buffer.clear(0, frames);
        if (channel->input) {
            channel->input->pull(channel->buffer, frames);
        }
    }
    for (auto& output : outputs) {
        if (output->buffer.getNumSamples() < frames) {
            output->buffer.setSize(2, frames, true, true, true);
        }
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

        const float sendGain = decibelsToGain(route->sendGainDb.get());
        if (sendGain == 0.0f) {
            continue;
        }
        for (int side = 0; side < 2; ++side) {
            (*outputIt)->buffer.addFrom(side, 0, (*channelIt)->buffer, side, 0, frames, sendGain);
        }
    }

    for (auto& output : outputs) {
        juce::AudioBuffer<float> block(output->buffer.getArrayOfWritePointers(), 2, 0, frames);
        for (auto& insert : output->inserts) {
            insert->pluginHost.processBlock(block, insert->bypassed.get());
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

    for (auto& output : outputs) {
        if (output->output) {
            output->output->push(output->buffer, frames);
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
        route->sendGainDb = -60.0f;
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

void AudioEngine::setSendGain(std::uint32_t channelId, std::uint32_t outputId, float gainDb) {
    for (auto& route : routes) {
        if (route->channelId == channelId && route->outputId == outputId) {
            route->sendGainDb = gainDb;
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
        savedChannel.inputChannels = {0, 1};
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
        for (const auto& insert : output->inserts) {
            savedOutput.inserts.push_back({insert->id, insert->pluginPath.toStdString(),
                                           insert->pluginName.toStdString(), insert->bypassed.get()});
        }
        scene.outputs.push_back(std::move(savedOutput));
    }
    for (const auto& route : routes) {
        scene.routes.push_back({route->channelId, route->outputId, route->sendGainDb.get()});
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
        route->sendGainDb = savedRoute.sendGainDb;
        restoredRoutes.push_back(std::move(route));
    }

    {
        const juce::ScopedLock lock(stateLock);
        channels = std::move(restoredChannels);
        outputs = std::move(restoredOutputs);
        routes = std::move(restoredRoutes);
        meters = std::move(restoredMeters);
    }

    // Re-apply device assignments where possible.
    for (const auto& [id, name] : savedInputDevices) {
        if (name.isNotEmpty()) {
            const auto e = setChannelInputDevice(id, name);
            if (e.isNotEmpty()) loadErrors += e + "\n";
        }
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

    if (deviceName == "None \xe2\x80\x94 silent" || deviceName.isEmpty()) {
        const juce::ScopedLock sl(stateLock);
        channel->input.reset();
        channel->inputDeviceName = "None \xe2\x80\x94 silent";
        channel->inputError.clear();
        return {};
    }

    const juce::String prevName = channel->inputDeviceName;
    auto prevInput = std::move(channel->input);

    auto newInput = std::make_unique<InputCapture>();
    juce::String err;
    const auto openErr = newInput->open(deviceName == "System Default" ? juce::String() : deviceName,
                                       currentSampleRate, err);
    if (openErr.isNotEmpty()) {
        const juce::ScopedLock sl(stateLock);
        channel->input = std::move(prevInput);
        channel->inputError = openErr;
        return openErr;
    }

    {
        const juce::ScopedLock sl(stateLock);
        channel->input = std::move(newInput);
        channel->inputDeviceName = deviceName;
        channel->inputError.clear();
    }
    return {};
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

    if (deviceName == "None \xe2\x80\x94 silent" || deviceName.isEmpty()) {
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
                                          rateForValidation, err);
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
            currentBufferSize = newBuf;
            clockOutputId = outputId;
            for (auto& channel : channels) {
                channel->buffer.setSize(2, currentBufferSize);
                channel->buffer.clear();
                for (auto& insert : channel->inserts) {
                    insert->pluginHost.prepareToPlay(currentSampleRate, currentBufferSize);
                }
            }
            for (auto& o : outputs) {
                o->buffer.setSize(2, currentBufferSize);
                o->buffer.clear();
                for (auto& insert : o->inserts) {
                    insert->pluginHost.prepareToPlay(currentSampleRate, currentBufferSize);
                }
            }
        }
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

juce::String AudioEngine::getClockDescription() const {
    if (clockOutputId == 0) {
        return "No output device \xe2\x80\x94 click a master strip";
    }
    for (const auto& o : outputs) {
        if (o->id == clockOutputId) {
            const auto device = o->outputDeviceName.isEmpty() ? juce::String("System Default")
                                                              : o->outputDeviceName;
            const auto khzValue = currentSampleRate / 1000.0;
            const auto khz = std::abs(khzValue - std::round(khzValue)) < 0.001
                                 ? juce::String((int)std::round(khzValue))
                                 : juce::String(khzValue, 1);
            return o->name + "  \xc2\xb7  " + device + "  \xc2\xb7  " + khz + " kHz  \xc2\xb7  "
                   + juce::String(currentBufferSize) + " smp";
        }
    }
    return "No output device \xe2\x80\x94 click a master strip";
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
    const juce::ScopedLock lock(stateLock);

    const Channel* inputChannel = nullptr;
    for (const auto& channel : channels) {
        if (channel->input != nullptr) {
            inputChannel = channel.get();
            break;
        }
    }

    int inputSamples = 0;
    int pluginSamples = 0;
    if (inputChannel != nullptr && inputChannel->input != nullptr) {
        inputSamples = inputChannel->input->getLatencySamples();
        for (const auto& insert : inputChannel->inserts) {
            pluginSamples += insert->pluginHost.getLatencySamples();
        }
    }

    int outputSamples = 0;
    bool valid = false;
    if (clockOutputId != 0) {
        for (const auto& output : outputs) {
            if (output->id == clockOutputId && output->output != nullptr) {
                outputSamples = output->output->getLatencySamples();
                for (const auto& insert : output->inserts) {
                    pluginSamples += insert->pluginHost.getLatencySamples();
                }
                valid = true;
                break;
            }
        }
    }

    const int totalSamples = inputSamples + pluginSamples + outputSamples;
    const double sampleRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    report.inputMs = inputSamples / sampleRate * 1000.0;
    report.pluginMs = pluginSamples / sampleRate * 1000.0;
    report.outputMs = outputSamples / sampleRate * 1000.0;
    report.totalMs = totalSamples / sampleRate * 1000.0;
    report.valid = valid;
    return report;
}

float AudioEngine::decibelsToGain(float gainDb) {
    return gainDb <= -60.0f ? 0.0f : std::pow(10.0f, gainDb / 20.0f);
}
