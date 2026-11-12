#pragma once

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "AudioEngine.h"
#include "LatencyBudgetView.h"
#include "PageHeader.h"
#include "Theme.h"
#include "WrappedLabel.h"

class SettingsView final : public juce::Component, private juce::Timer {
public:
    explicit SettingsView(AudioEngine& engine);

    void resized() override;
    void refresh();

    std::function<void(bool)> onCloseToTrayChanged;
    void setCloseToTrayEnabled(bool enabled);

private:
    void timerCallback() override;
    void applyPending();
    void revertPending();
    void updatePendingState();
    void refreshReloadDevices();
    void pollAutoReload();
    void pushWatchedDevices();
    void syncWatchedFromEngine();
    void refreshListenChoices();
    void updateMeasurement(const AudioEngine::LatencyReport& report);
    juce::String listenTarget() const;
    // Returns true when the text actually changed, so refresh() can re-run the
    // layout only when a wrapped line grew or shrank.
    bool setWrappedText(WrappedLabel& label, const juce::String& text, juce::Colour colour);
    void layoutBodyToFit();
    int layoutBody(int width);
    AudioEngine& audioEngine;

    // The page body scrolls under a pinned header: the six sections do not fit
    // a 700 px tall window, and the device list at the bottom has to stay
    // reachable. Panels are painted here so the controls above them stay plain
    // JUCE widgets.
    class Body final : public juce::Component {
    public:
        void setPanels(std::vector<juce::Rectangle<int>> panels) {
            panels_ = std::move(panels);
            repaint();
        }

        void paint(juce::Graphics& g) override {
            for (const auto& panel : panels_)
                theme::ui::drawPanel(g, panel);
        }

    private:
        std::vector<juce::Rectangle<int>> panels_;
    };

    PageHeader header{ "Settings" };
    Body body;
    juce::Viewport viewport;

    juce::Label statusCaption{ "", "AUDIO STATUS" };
    juce::Label clockStatCaption{ "", "CLOCK" };
    juce::Label clockStatValue;
    juce::Label latencyStatCaption{ "", "LATENCY (EST.)" };
    juce::Label latencyStatValue;
    juce::Label bufferStatCaption{ "", "BUFFER" };
    juce::Label bufferStatValue;
    WrappedLabel issuesText;

    juce::Label bufferCaption{ "", "BUFFER SIZE" };
    juce::Label inputBufferSizeLabel;
    juce::ComboBox inputBufferSizeCombo;
    juce::Label outputBufferSizeLabel;
    juce::ComboBox outputBufferSizeCombo;

    juce::Label pendingLabel;
    juce::TextButton applyButton { "Apply" };
    juce::TextButton revertButton { "Revert" };

    // Round-trip measurement needs a physical cable from the clock output back
    // into an input, so the control names both ends of that cable.
    juce::Label roundTripLabel;
    juce::Label roundTripBeta { "", "BETA" };
    juce::Label measuredLabel;
    juce::Label measuredDetailLabel;
    LatencyBudgetView budgetView;
    juce::Label renderLoadLabel;
    juce::Label listenLabel { "", "Listen on" };
    juce::ComboBox listenCombo;
    juce::TextButton measureButton { "Measure Round-Trip" };
    juce::String listenSelection_;
    juce::StringArray listenNames_;
    bool listenChoicesBuilt_ = false;

    juce::Label tuningLabel;
    juce::Label tuningBeta { "", "BETA" };
    juce::ToggleButton lowLatencyInputToggle { "Low-latency input (1-block read delay)" };
    juce::ToggleButton exclusiveToggle { "Prefer exclusive mode" };
    juce::ToggleButton lowLatencyOutputsToggle { "Low-latency outputs (2-block FIFO)" };
    juce::ToggleButton minimalClockFifoToggle { "Minimal clock FIFO" };
    juce::ToggleButton fullDuplexToggle { "Same-device input (one-block start)" };
    // Scene-v6 hardening. Off by default so a pre-v6 save behaves exactly as
    // before; the switches save with the session once they are turned on.
    juce::ToggleButton slicedRenderToggle { "Deadline-aware sliced rendering" };
    juce::ToggleButton scrubToggle { "Scrub non-finite samples at inserts" };
    juce::ToggleButton healthReloadToggle { "Reload streams that stay yellow/red" };
    juce::ToggleButton ladderRecoveryToggle { "Allow fallback ladder to recover one rung" };
    // Preset rows for the health thresholds; the exact values travel with the
    // scene, so a session restores the sensitivity it was left with.
    juce::Label healthSensitivityLabel { "", "Health sensitivity" };
    juce::ComboBox healthSensitivityCombo;
    juce::TextButton profileButton { "Find low-latency setting" };
    WrappedLabel profileStatusLabel;
    juce::TextButton forceAsioButton { "Force ASIO" };
    WrappedLabel asioStatusLabel;
    juce::String forceAsioStatus_;

    juce::Label appCaption{ "", "APP" };

    // Rack view cost control: the thumbnail timer's rate. Applied straight to
    // the engine (no Apply step) because it is not a stream option.
    juce::Label rackThumbnailsLabel { "", "Rack thumbnails" };
    juce::ComboBox rackThumbnailFpsCombo;
    // Which inserts keep live previews: focused card only, slow rotation, or
    // one hidden editor per insert. Applied straight to the engine too.
    juce::Label rackPreviewModeLabel { "", "Background previews" };
    juce::ComboBox rackPreviewModeCombo;
    // How long a meter holds its peak line. Session-wide, applied at once.
    juce::Label peakHoldLabel { "", "Peak hold" };
    juce::ComboBox peakHoldCombo;
    juce::ToggleButton closeToTrayToggle { "Minimize to system tray when closed" };

    juce::Label devicesLabel;
    juce::TextButton reloadAllButton { "Reload All Audio Devices" };
    juce::Viewport reloadDevicesViewport;
    juce::Component reloadDevicesHolder;
    juce::OwnedArray<juce::ToggleButton> reloadDeviceToggles;
    juce::StringArray selectedReloadDevices_;
    // Replaces the old manual "reload selected" button: when a watched device
    // reports an error it is reopened on its own, up to a few rounds apart.
    juce::ToggleButton autoReloadToggle { "Auto-reload checked devices on error" };
    // Per watched strip: last automatic attempt (ms) and how many rounds it
    // has had, so a permanently broken device cannot spin forever.
    std::map<std::pair<bool, std::uint32_t>, std::pair<std::uint32_t, int>> autoReloadState_;

    // Staged settings: nothing reaches the engine until Apply.
    AudioEngine::DeviceSettings staged_;
    bool dirty_ = false;
    // The latency budget draws nothing until a clock is open; the panel
    // collapses rather than leaving a blank block of rows.
    bool hasBudget_ = false;
    std::vector<AudioEngine::ReloadableDevice> reloadDevices_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsView)
};
