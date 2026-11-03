#pragma once

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "AudioEngine.h"
#include "PageHeader.h"
#include "Theme.h"

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
    AudioEngine& audioEngine;

    PageHeader header{ "Settings" };
    juce::Label clockLabel;
    juce::Label latencyLabel;
    juce::Label bufferLabel;
    juce::Label errorLabel;

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

    // Rack view cost control: the thumbnail timer's rate. Applied straight to
    // the engine (no Apply step) because it is not a stream option.
    juce::Label rackThumbnailsLabel { "", "Rack thumbnails" };
    juce::ComboBox rackThumbnailFpsCombo;
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
    std::vector<AudioEngine::ReloadableDevice> reloadDevices_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsView)
};
