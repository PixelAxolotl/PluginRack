#pragma once

#include <JuceHeader.h>

struct SavedScene {
    juce::String pluginPath;
    juce::String pluginName;
    double sampleRate = 48000.0;
    int bufferFrames = 256;
};

class SceneBridge {
public:
    static juce::File getSceneFile();
    static juce::String save(const juce::String& pluginPath, const juce::String& pluginName,
                             double sampleRate, int bufferFrames);
    static std::optional<SavedScene> load(juce::String& errorMessage);
};
