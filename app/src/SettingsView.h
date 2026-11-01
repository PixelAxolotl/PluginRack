#pragma once

#include <JuceHeader.h>

#include <vector>

#include "AudioEngine.h"
#include "Theme.h"

class SettingsView final : public juce::Component, private juce::Timer {
public:
    explicit SettingsView(AudioEngine& engine);

    void resized() override;
    void refresh();

private:
    void timerCallback() override;
    void applyPending();
    void revertPending();
    void updatePendingState();
    void refreshReloadDevices();
    AudioEngine& audioEngine;

    juce::Label titleLabel;
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

    juce::TextButton measureButton { "Measure Round-Trip Latency" };
    juce::Label measuredLabel;

    juce::Label tuningLabel;
    juce::ToggleButton lowLatencyInputToggle { "Low-latency input (1-block read delay)" };
    juce::ToggleButton exclusiveToggle { "Prefer exclusive mode" };
    juce::ToggleButton lowLatencyOutputsToggle { "Low-latency outputs (2-block FIFO)" };
    juce::ToggleButton minimalClockFifoToggle { "Minimal clock FIFO" };
    juce::ToggleButton fullDuplexToggle { "Same-device input (full duplex)" };

    juce::Label devicesLabel;
    juce::TextButton reloadAllButton { "Reload All Audio Devices" };
    juce::Viewport reloadDevicesViewport;
    juce::Component reloadDevicesHolder;
    juce::OwnedArray<juce::ToggleButton> reloadDeviceToggles;
    juce::StringArray selectedReloadDevices_;
    juce::TextButton reloadDevicesButton { "Reload Selected Devices" };

    // Staged settings: nothing reaches the engine until Apply.
    AudioEngine::DeviceSettings staged_;
    bool dirty_ = false;
    std::vector<AudioEngine::ReloadableDevice> reloadDevices_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsView)
};