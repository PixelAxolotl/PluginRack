#include "SceneBridge.h"

#include <pluginrack/SceneCodec.h>
#include <pluginrack/SceneValidator.h>

juce::File SceneBridge::getSceneFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("PluginRack")
        .getChildFile("scenes")
        .getChildFile("default.json");
}

juce::String SceneBridge::save(const pluginrack::Scene& scene) {
    const auto errors = pluginrack::validate(scene);
    if (!errors.empty()) {
        return "Could not save the scene: " + juce::String(errors.front().message);
    }

    const auto file = getSceneFile();
    const auto directoryResult = file.getParentDirectory().createDirectory();
    if (directoryResult.failed()) {
        return directoryResult.getErrorMessage();
    }
    if (!file.replaceWithText(pluginrack::toJson(scene))) {
        return "Could not write the scene to " + file.getFullPathName();
    }
    return {};
}

std::optional<pluginrack::Scene> SceneBridge::load(juce::String& errorMessage) {
    errorMessage.clear();
    const auto file = getSceneFile();
    if (!file.existsAsFile()) {
        return std::nullopt;
    }

    auto result = pluginrack::fromJson(file.loadFileAsString().toStdString());
    if (!result.errors.empty()) {
        errorMessage = "Could not load the saved scene: " + juce::String(result.errors.front());
        return std::nullopt;
    }
    if (!result.scene.has_value()) {
        errorMessage = "The saved scene is empty.";
        return std::nullopt;
    }
    return std::move(result.scene);
}
