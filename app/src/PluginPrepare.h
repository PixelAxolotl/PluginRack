#pragma once

#include <JuceHeader.h>

// One prepare sequence for every place a plugin instance is handed a format.
// The app prepares on its own thread and the child host prepares on its audio
// thread; a plugin that reseeds state in prepareToPlay must see the same calls
// in both, or the in-process and child renderers drift apart.
namespace pluginprepare {

inline void prepareInstance(juce::AudioPluginInstance& instance, double sampleRate,
                            int blockSize) {
    instance.releaseResources();
    instance.setRateAndBufferSizeDetails(sampleRate, blockSize);
    instance.prepareToPlay(sampleRate, blockSize);
}

} // namespace pluginprepare
