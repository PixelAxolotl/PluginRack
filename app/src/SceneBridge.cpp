#include "SceneBridge.h"

#include <pluginrack/SceneCodec.h>
#include <pluginrack/SceneValidator.h>

namespace {
std::vector<juce::File> lastSceneCandidates() {
    const auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("PluginRack")
                         .getChildFile("scenes");
    return {
        dir.getChildFile("last.json"),
        dir.getChildFile("autosave.json"),
        dir.getChildFile("session.json"),
        dir.getChildFile("previous.json"),
        dir.getChildFile("lastscene.json")
    };
}

bool isReservedSaveName(const juce::String& name) {
    const juce::String lower = name.toLowerCase();
    // "default" is the scene file SceneBridge::save() writes; a named save of
    // the same name would overwrite or delete it.
    return lower == "last" || lower == "autosave" || lower == "session"
        || lower == "previous" || lower == "lastscene" || lower == "default"
        || lower == "default_save";
}

juce::String sanitizeSaveName(const juce::String& name) {
    juce::String result = name.trim();
    // Strip any path components / extension supplied by the user.
    result = result.replaceCharacter('\\', '_').replaceCharacter('/', '_');
    // Windows rejects these in a file name; replacing them keeps the save
    // from failing with an unexplained create error.
    for (const auto character : juce::String(":*?\"<>|")) {
        result = result.replaceCharacter(character, '_');
    }
    // Trailing dots and spaces are dropped by Windows, so drop them here too.
    while (result.endsWithChar('.') || result.endsWithChar(' ')) {
        result = result.dropLastCharacters(1);
    }
    if (result.endsWithIgnoreCase(".json")) result = result.dropLastCharacters(5);
    return result;
}
}

juce::File SceneBridge::getScenesDirectory() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("PluginRack")
        .getChildFile("scenes");
}

juce::File SceneBridge::getSceneFile() {
    return getScenesDirectory().getChildFile("default.json");
}

juce::File SceneBridge::getLastSceneFile() {
    for (const auto& file : lastSceneCandidates()) {
        if (file.existsAsFile()) return file;
    }
    return lastSceneCandidates().front();
}

juce::StringArray SceneBridge::getSaveNames() {
    juce::StringArray names;
    const auto dir = getScenesDirectory();
    if (!dir.exists()) return names;
    // The auto-saved session files and the default scene are not user saves.
    std::vector<juce::File> reserved = lastSceneCandidates();
    reserved.push_back(getSceneFile());
    for (const auto& file : dir.findChildFiles(juce::File::findFiles, false, "*.json")) {
        bool isReserved = false;
        for (const auto& candidate : reserved) {
            if (file.getFullPathName() == candidate.getFullPathName()) {
                isReserved = true;
                break;
            }
        }
        if (isReserved) continue;
        names.add(file.getFileNameWithoutExtension());
    }
    names.sort(true);
    return names;
}

juce::File SceneBridge::getSaveFile(const juce::String& name) {
    return getScenesDirectory().getChildFile(sanitizeSaveName(name) + ".json");
}

juce::String SceneBridge::save(const pluginrack::Scene& scene) {
    return save(scene, getSceneFile());
}

juce::String SceneBridge::save(const pluginrack::Scene& scene, const juce::File& file) {
    const auto errors = pluginrack::validate(scene);
    if (!errors.empty()) {
        return "Could not save the scene: " + juce::String(errors.front().message);
    }

    const auto directoryResult = file.getParentDirectory().createDirectory();
    if (directoryResult.failed()) {
        return directoryResult.getErrorMessage();
    }
    if (!file.replaceWithText(pluginrack::toJson(scene))) {
        return "Could not write the scene to " + file.getFullPathName();
    }
    return {};
}

juce::String SceneBridge::saveNamed(const pluginrack::Scene& scene, const juce::String& name) {
    const auto clean = sanitizeSaveName(name);
    if (clean.isEmpty()) return "Please enter a save name.";
    if (isReservedSaveName(clean)) return "That name is reserved. Please choose another.";

    return save(scene, getSaveFile(clean));
}

juce::String SceneBridge::saveLast(const pluginrack::Scene& scene) {
    const auto errors = pluginrack::validate(scene);
    if (!errors.empty()) {
        return "Could not save the scene: " + juce::String(errors.front().message);
    }

    juce::String firstError;
    bool anySuccess = false;
    for (const auto& file : lastSceneCandidates()) {
        const auto directoryResult = file.getParentDirectory().createDirectory();
        if (directoryResult.failed()) {
            if (firstError.isEmpty()) firstError = directoryResult.getErrorMessage();
            continue;
        }
        if (!file.replaceWithText(pluginrack::toJson(scene))) {
            if (firstError.isEmpty()) firstError = "Could not write the scene to " + file.getFullPathName();
            continue;
        }
        anySuccess = true;
    }
    if (!anySuccess) {
        return firstError.isNotEmpty() ? firstError : "Could not save the scene.";
    }
    return {};
}

bool SceneBridge::deleteSave(const juce::String& name) {
    const auto clean = sanitizeSaveName(name);
    if (clean.isEmpty() || isReservedSaveName(clean)) return false;
    const auto file = getSaveFile(clean);
    if (!file.existsAsFile()) return false;
    const bool ok = file.deleteFile();
    if (ok && getDefaultSaveName().equalsIgnoreCase(clean)) setDefaultSaveName({});
    return ok;
}

juce::String SceneBridge::renameSave(const juce::String& oldName, const juce::String& newName) {
    const auto oldClean = sanitizeSaveName(oldName);
    const auto newClean = sanitizeSaveName(newName);
    if (oldClean.isEmpty() || newClean.isEmpty()) return "Please enter a valid name.";
    if (isReservedSaveName(newClean)) return "That name is reserved. Please choose another.";
    if (oldClean.equalsIgnoreCase(newClean)) return {};

    const auto oldFile = getSaveFile(oldClean);
    const auto newFile = getSaveFile(newClean);
    if (!oldFile.existsAsFile()) return "The save does not exist.";
    if (newFile.existsAsFile()) return "A save with that name already exists.";

    const auto dirResult = newFile.getParentDirectory().createDirectory();
    if (dirResult.failed()) return dirResult.getErrorMessage();
    if (!oldFile.moveFileTo(newFile)) return "Could not rename the save.";

    if (getDefaultSaveName().equalsIgnoreCase(oldClean)) setDefaultSaveName(newClean);
    return {};
}

juce::String SceneBridge::moveSave(const juce::String& name, const juce::File& destinationDirectory) {
    const auto clean = sanitizeSaveName(name);
    if (clean.isEmpty() || isReservedSaveName(clean)) return "Invalid save name.";
    const auto source = getSaveFile(clean);
    if (!source.existsAsFile()) return "The save does not exist.";
    if (!destinationDirectory.exists() && !destinationDirectory.createDirectory().wasOk())
        return "Could not create the destination folder.";

    const auto destination = destinationDirectory.getChildFile(source.getFileName());
    if (destination.existsAsFile()) return "A file with that name already exists in the destination.";
    if (!source.moveFileTo(destination)) return "Could not move the save.";

    if (getDefaultSaveName().equalsIgnoreCase(clean)) setDefaultSaveName({});
    return {};
}

juce::String SceneBridge::getDefaultSaveName() {
    const auto file = getScenesDirectory().getChildFile("default_save.txt");
    if (!file.existsAsFile()) return {};
    return file.loadFileAsString().trim();
}

void SceneBridge::setDefaultSaveName(const juce::String& name) {
    const auto dirResult = getScenesDirectory().createDirectory();
    if (dirResult.failed()) return;
    const auto file = getScenesDirectory().getChildFile("default_save.txt");
    if (name.trim().isEmpty()) {
        file.deleteFile();
        return;
    }
    file.replaceWithText(sanitizeSaveName(name));
}

std::optional<pluginrack::Scene> SceneBridge::load(juce::String& errorMessage) {
    return load(getSceneFile(), errorMessage);
}

std::optional<pluginrack::Scene> SceneBridge::load(const juce::File& file, juce::String& errorMessage) {
    errorMessage.clear();
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

std::optional<pluginrack::Scene> SceneBridge::loadNamed(const juce::String& name, juce::String& errorMessage) {
    const auto clean = sanitizeSaveName(name);
    if (clean.isEmpty()) {
        errorMessage = "Please enter a save name.";
        return std::nullopt;
    }
    return load(getSaveFile(clean), errorMessage);
}

std::optional<pluginrack::Scene> SceneBridge::loadLast(juce::String& errorMessage) {
    errorMessage.clear();
    juce::String firstError;
    bool anyExisting = false;
    for (const auto& file : lastSceneCandidates()) {
        if (!file.existsAsFile()) continue;
        anyExisting = true;
        auto result = pluginrack::fromJson(file.loadFileAsString().toStdString());
        if (!result.errors.empty()) {
            if (firstError.isEmpty()) firstError = "Could not load the saved scene: " + juce::String(result.errors.front());
            continue;
        }
        if (!result.scene.has_value()) {
            if (firstError.isEmpty()) firstError = "The saved scene is empty.";
            continue;
        }
        return std::move(result.scene);
    }
    if (anyExisting && firstError.isNotEmpty()) {
        errorMessage = firstError;
    }
    return std::nullopt;
}

std::optional<pluginrack::Scene> SceneBridge::loadDefaultOrLast(juce::String& errorMessage) {
    errorMessage.clear();
    const auto defaultName = getDefaultSaveName();
    if (defaultName.isNotEmpty()) {
        auto scene = loadNamed(defaultName, errorMessage);
        if (!scene.has_value() && errorMessage.isEmpty()) {
            errorMessage = "The selected default save '" + defaultName +
                            "' could not be loaded.";
        }
        return scene;
    }
    return loadLast(errorMessage);
}