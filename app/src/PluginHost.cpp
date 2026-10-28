#include "PluginHost.h"

PluginHost::PluginHost() {
    formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());
}

juce::String PluginHost::loadPlugin(const juce::File& file) {
    if (!file.exists()) {
        return "The selected VST3 does not exist.";
    }

    double sampleRate = 0.0;
    int bufferSize = 0;
    {
        const juce::ScopedLock lock(pluginLock);
        sampleRate = currentSampleRate;
        bufferSize = currentBufferSize;
    }

    juce::OwnedArray<juce::PluginDescription> descriptions;
    formatManager.getFormat(0)->findAllTypesForFile(descriptions, file.getFullPathName());
    if (descriptions.isEmpty()) {
        return "No VST3 plugin was found in the selected file.";
    }

    juce::String errorMessage;
    auto candidate = formatManager.createPluginInstance(*descriptions[0], sampleRate, bufferSize,
                                                        errorMessage);
    if (candidate == nullptr) {
        return errorMessage.isNotEmpty() ? errorMessage : "The VST3 could not be instantiated.";
    }

    auto layout = candidate->getBusesLayout();
    if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty()) {
        return "The selected plugin is not an audio effect with input and output buses.";
    }

    layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (!candidate->setBusesLayout(layout)) {
        return "The selected plugin does not support stereo input and output.";
    }

    candidate->setRateAndBufferSizeDetails(sampleRate, bufferSize);
    candidate->prepareToPlay(sampleRate, bufferSize);

    const juce::ScopedLock lock(pluginLock);
    if (plugin != nullptr) {
        plugin->releaseResources();
    }
    pluginPath = file.getFullPathName();
    pluginName = candidate->getName();
    plugin = std::move(candidate);
    return {};
}

void PluginHost::prepareToPlay(double sampleRate, int samplesPerBlock) {
    const juce::ScopedLock lock(pluginLock);
    currentSampleRate = sampleRate;
    currentBufferSize = samplesPerBlock;
    if (plugin != nullptr) {
        plugin->setRateAndBufferSizeDetails(sampleRate, samplesPerBlock);
        plugin->prepareToPlay(sampleRate, samplesPerBlock);
    }
}

void PluginHost::releaseResources() {
    const juce::ScopedLock lock(pluginLock);
    if (plugin != nullptr) {
        plugin->releaseResources();
    }
}

void PluginHost::processBlock(juce::AudioBuffer<float>& buffer, bool bypassed) {
    if (bypassed) {
        return;
    }

    const juce::ScopedTryLock lock(pluginLock);
    if (!lock.isLocked() || plugin == nullptr) {
        return;
    }

    midiBuffer.clear();
    plugin->processBlock(buffer, midiBuffer);
}

juce::String PluginHost::getPluginPath() const {
    const juce::ScopedLock lock(pluginLock);
    return pluginPath;
}

juce::String PluginHost::getPluginName() const {
    const juce::ScopedLock lock(pluginLock);
    return pluginName;
}

double PluginHost::getSampleRate() const {
    const juce::ScopedLock lock(pluginLock);
    return currentSampleRate;
}

int PluginHost::getBufferSize() const {
    const juce::ScopedLock lock(pluginLock);
    return currentBufferSize;
}
