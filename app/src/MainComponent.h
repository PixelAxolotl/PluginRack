#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>

#include "AudioEngine.h"
#include "IoView.h"
#include "MixerComponent.h"
#include "RackView.h"
#include "RoutingMatrixComponent.h"
#include "SavesView.h"
#include "SettingsView.h"
#include "Theme.h"

enum class AppView { Rack = 0, Mixer, Routing, IO, Saves, Settings };

// Persistent 56 px header: wordmark, view navigation, snapshot placeholder,
// audio status, error affordance, scene buttons. Paints the same bar the
// MainComponent used to paint directly.
class ToolbarComponent final : public juce::Component {
public:
    ToolbarComponent();

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setView(AppView view);
    void setClockText(const juce::String& text, bool isWarning);
    void setLatencyText(const juce::String& text, juce::Colour colour, const juce::String& tooltip);
    void setHasError(bool hasError);

    std::function<void(AppView)> onViewSelected;
    std::function<void()> onSave;
    std::function<void()> onMenu;
    std::function<void()> onSnapshot;
    std::function<void()> onErrorClicked;

    juce::Component* getMenuTarget() { return &menuButton; }

private:
    void selectView(AppView view);
    void updateNavToggles();

    juce::TextButton racksButton{"Racks"};
    juce::TextButton mixerButton{"Mixer"};
    juce::TextButton routingButton{"Routing"};
    juce::TextButton ioButton{"I/O"};
    juce::TextButton settingsButton{"Settings"};
    juce::TextButton snapshotButton{"Snapshot"};
    juce::Label latencyLabel;
    juce::TextButton errorButton{"!"};
    juce::TextButton saveButton{"Save"};
    juce::TextButton savesButton{"Saves"};
    juce::TextButton menuButton{"Menu"};
    juce::String clockText;
    juce::Colour clockColour = theme::dim();
    juce::Rectangle<int> clockBounds;
    AppView currentView = AppView::Rack;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToolbarComponent)
};

// Content area below the toolbar. Hosts the real mixer and rack views plus
// lightweight placeholders for the views later tasks will build out. Default is Rack.
class ViewRouter final : public juce::Component {
public:
    ViewRouter(AudioEngine& engine, MixerComponent& mixerView);

    void setView(AppView view);
    AppView getView() const { return currentView; }
    void refreshViews();
    // Device state changed underneath the strips (a background open finished):
    // refresh the device readouts in place so a missing device shows red.
    void refreshDeviceStrips();
    SettingsView& getSettingsView() { return settingsView; }
    RackView& getRackView() { return rackView; }
    void resized() override;

    std::function<void(AppView)> onViewChanged;
    std::function<void()> onStructureChanged;
    // Wired by MainComponent to its synchronous scene apply. The Saves view
    // parses save files but never touches the engine: restoreScene mutates
    // plugins and JUCE components that belong to the message thread, so the
    // load request round-trips through the host.
    std::function<juce::String(const pluginrack::Scene&)> onRestoreRequested;

    // MainComponent calls this before engine strip mutation: the rack's
    // docked plugin editor is backed by a strip's plugin instance, and an
    // editor whose processor dies first is unsafe to delete.
    void releaseDockedEditor() { rackView.closeDockedEditor(); }
    void releaseOrphanedEditor() { rackView.releaseOrphanedDockedEditor(); }

private:
    class PlaceholderView final : public juce::Component {
    public:
        PlaceholderView(const juce::String& title, const juce::String& subtitle);
        void setSubtitle(const juce::String& subtitle);
        void paint(juce::Graphics& g) override;
        void resized() override;

    private:
        juce::Label titleLabel;
        juce::Label subtitleLabel;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlaceholderView)
    };

    AudioEngine& audioEngine;
    MixerComponent& mixerView;
    RackView rackView;
    RoutingMatrixComponent routingView;
    IoView ioView;
    SavesView savesView;
    SettingsView settingsView;
    AppView currentView = AppView::Rack;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ViewRouter)
};

class MainComponent final : public juce::Component, private juce::Timer {
public:
    MainComponent();
    ~MainComponent() override = default;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void selectView(AppView view);
    void saveSceneOnExit();
    void initialiseAudio();
    SettingsView& settings() { return router.getSettingsView(); }
    RackView& rack() { return router.getRackView(); }

private:
    void saveScene();
    void saveSceneAs();
    void loadScene(bool showMissingMessage);
    // Applies a parsed scene on the message thread and returns plugin load
    // errors. Guarded by sceneLoading_; callers run it directly (the strip
    // swap itself is brief). openDevicesNow=false defers the stream opens to
    // the engine's startup worker so the UI is not blocked by slow drivers.
    juce::String applyScene(const pluginrack::Scene& scene, bool openDevicesNow = true);
    void showSavesMenu();
    void showMenu();
    void showAudioErrors();
    void showSnapshotPlaceholder();
    void refreshViews();
    void showError(const juce::String& message);
    void timerCallback() override;

    std::unique_ptr<juce::FileChooser> fileChooser;
    AudioEngine audioEngine;
    MixerComponent mixer;
    ToolbarComponent toolbar;
    ViewRouter router;
    double smoothedLatencyMs = -1.0;
    std::atomic<bool> sceneLoading_{false};
    // Counts UI ticks between device-availability checks (see timerCallback).
    int availabilityTicks_ = 0;
    // Startup: the audio restore is queued from the first paint, never from
    // the constructor, so the window is on screen before any device work.
    bool audioInitQueued_ = false;
    // Same rule for the plugin scan: it spawns a child process per bundle.
    bool pluginScanQueued_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
