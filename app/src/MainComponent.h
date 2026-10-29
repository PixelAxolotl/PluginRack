#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "MixerComponent.h"
#include "RackView.h"
#include "RoutingMatrixComponent.h"
#include "SettingsView.h"
#include "Theme.h"

enum class AppView { Rack = 0, Mixer, Routing, Settings };

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
    std::function<void()> onLoad;
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
    juce::TextButton settingsButton{"Settings"};
    juce::TextButton snapshotButton{"Snapshot"};
    juce::Label latencyLabel;
    juce::TextButton errorButton{"!"};
    juce::TextButton saveButton{"Save"};
    juce::TextButton loadButton{"Load"};
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
    void resized() override;

    std::function<void(AppView)> onViewChanged;
    std::function<void()> onStructureChanged;

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

private:
    void saveScene();
    void loadScene(bool showMissingMessage);
    void showMenu();
    void showAudioErrors();
    void showSnapshotPlaceholder();
    void refreshViews();
    void showError(const juce::String& message);
    void timerCallback() override;

    AudioEngine audioEngine;
    MixerComponent mixer;
    ToolbarComponent toolbar;
    ViewRouter router;
    double smoothedLatencyMs = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
