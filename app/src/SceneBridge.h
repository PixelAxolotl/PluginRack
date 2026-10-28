#pragma once

#include <JuceHeader.h>

#include <optional>

#include <pluginrack/Scene.h>

class SceneBridge {
public:
    static juce::File getSceneFile();
    static juce::String save(const pluginrack::Scene& scene);
    static std::optional<pluginrack::Scene> load(juce::String& errorMessage);
};
