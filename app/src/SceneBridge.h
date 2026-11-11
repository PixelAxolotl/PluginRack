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

    // The exit autosave runs in shutdown(), immediately before the process is
    // terminated, so it cannot open a dialog. The failure is left here instead
    // and shown on the next launch: without it a session that could not be
    // written would silently come back as the previous one.
    static void writeSaveFailureNote(const juce::String& error);
    // Returns the pending note and deletes it, so it is shown exactly once.
    static juce::String takeSaveFailureNote();

    static std::optional<pluginrack::Scene> load(juce::String& errorMessage);
    static std::optional<pluginrack::Scene> load(const juce::File& file, juce::String& errorMessage);
    // Trust-gated load (PR-SEC-010): like load(), but also reports whether
    // the scene names plugins outside the scanned library (untrustedPlugins)
    // and whether the file lives outside the app's scenes folder
    // (outsideScenesFolder — such scenes start with plugins disabled).
    static std::optional<pluginrack::Scene> loadTrusted(const juce::File& file,
                                                        juce::String& errorMessage,
                                                        bool& untrustedPlugins,
                                                        bool& outsideScenesFolder);
    static std::optional<pluginrack::Scene> loadNamed(const juce::String& name, juce::String& errorMessage);
    static std::optional<pluginrack::Scene> loadLast(juce::String& errorMessage);
    static std::optional<pluginrack::Scene> loadAutosave(juce::String& errorMessage) { return loadLast(errorMessage); }
    static std::optional<pluginrack::Scene> loadSession(juce::String& errorMessage) { return loadLast(errorMessage); }
    // Loads the user-selected default save if one is set, otherwise the last
    // autosaved scene.
    static std::optional<pluginrack::Scene> loadDefaultOrLast(juce::String& errorMessage);
};