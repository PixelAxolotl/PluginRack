#include "SceneBridge.h"

#include <pluginrack/SceneCodec.h>

juce::File SceneBridge::getSceneFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("PluginRack")
        .getChildFile("scenes")
        .getChildFile("default.json");
}

juce::String SceneBridge::save(const juce::String& pluginPath, const juce::String& pluginName,
                               double sampleRate, int bufferFrames) {
    pluginrack::Scene scene;
    scene.name = "Default";
    scene.sampleRate = sampleRate;
    scene.bufferFrames = bufferFrames;

    pluginrack::Channel channel;
    channel.id = 1;
    channel.name = "Channel 1";
    channel.layout = pluginrack::ChannelLayout::Stereo;
    if (pluginPath.isNotEmpty()) {
        channel.inserts.push_back({1, pluginPath.toStdString(), pluginName.toStdString(), false});
    }
    scene.channels.push_back(channel);

    pluginrack::Output output;
    output.id = 1;
    output.name = "Output 1";
    scene.outputs.push_back(output);
    scene.routes.push_back({1, 1, 0.0f});

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

std::optional<SavedScene> SceneBridge::load(juce::String& errorMessage) {
    errorMessage.clear();
    const auto file = getSceneFile();
    if (!file.existsAsFile()) {
        return std::nullopt;
    }

    const auto result = pluginrack::fromJson(file.loadFileAsString().toStdString());
    if (!result.errors.empty()) {
        errorMessage = juce::String("Could not load the saved scene: ")
                       + juce::String(result.errors.front());
        return std::nullopt;
    }
    if (!result.scene.has_value()) {
        errorMessage = "The saved scene is empty.";
        return std::nullopt;
    }

    SavedScene saved;
    saved.sampleRate = result.scene->sampleRate;
    saved.bufferFrames = result.scene->bufferFrames;
    for (const auto& channel : result.scene->channels) {
        if (!channel.inserts.empty()) {
            saved.pluginPath = channel.inserts.front().pluginPath;
            saved.pluginName = channel.inserts.front().pluginName;
            break;
        }
    }
    return saved;
}
