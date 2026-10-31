#pragma once

#include <JuceHeader.h>

#include <optional>
#include <vector>

#include <pluginrack/Scene.h>

class SceneBridge {
public:
    // The user-saved "default" scene file.
    static juce::File getSceneFile();
    // Directory containing all saved scenes.
    static juce::File getScenesDirectory();
    // The automatically saved scene from the previous session (used as a
    // fallback when no default scene has been saved yet).
    static juce::File getLastSceneFile();
    static juce::File getAutosaveFile() { return getLastSceneFile(); }
    static juce::File getSessionFile() { return getLastSceneFile(); }

    static juce::StringArray getSaveNames();

    static juce::File getSaveFile(const juce::String& name);

    static juce::String save(const pluginrack::Scene& scene);
    static juce::String save(const pluginrack::Scene& scene, const juce::File& file);
    static juce::String saveNamed(const pluginrack::Scene& scene, const juce::String& name);
    static juce::String saveLast(const pluginrack::Scene& scene);
    static juce::String saveAutosave(const pluginrack::Scene& scene) { return saveLast(scene); }
    static juce::String saveSession(const pluginrack::Scene& scene) { return saveLast(scene); }

    static bool deleteSave(const juce::String& name);
    static juce::String renameSave(const juce::String& oldName, const juce::String& newName);
    static juce::String moveSave(const juce::String& name, const juce::File& destinationDirectory);

    static juce::String getDefaultSaveName();
    static void setDefaultSaveName(const juce::String& name);

    static std::optional<pluginrack::Scene> load(juce::String& errorMessage);
    static std::optional<pluginrack::Scene> load(const juce::File& file, juce::String& errorMessage);
    static std::optional<pluginrack::Scene> loadNamed(const juce::String& name, juce::String& errorMessage);
    static std::optional<pluginrack::Scene> loadLast(juce::String& errorMessage);
    static std::optional<pluginrack::Scene> loadAutosave(juce::String& errorMessage) { return loadLast(errorMessage); }
    static std::optional<pluginrack::Scene> loadSession(juce::String& errorMessage) { return loadLast(errorMessage); }
    // Loads the user-selected default save if one is set, otherwise the last
    // autosaved scene.
    static std::optional<pluginrack::Scene> loadDefaultOrLast(juce::String& errorMessage);
};