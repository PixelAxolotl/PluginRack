#include "SettingsView.h"

#include <array>
#include <utility>

namespace {
constexpr int kBufferSizes[] = { 32, 64, 128, 256, 480, 512, 1024, 2048, 4096 };
constexpr int kAutoBufferItemId = 1; // ComboBox ids must be non-zero.

// The driver chooses the actual buffer size (often 480). Make sure the combo
// contains it so the current value is visible, not just the presets.
void showDeviceBuffer(juce::ComboBox& combo, int current) {
    if (current <= 0) {
        combo.setSelectedId(kAutoBufferItemId, juce::dontSendNotification);
        return;
    }
    const int wantedId = current + 1;
    if (combo.indexOfItemId(wantedId) < 0) {
        combo.clear(juce::dontSendNotification);
        combo.addItem("Auto (driver default)", kAutoBufferItemId);
        for (int size : kBufferSizes) {
            combo.addItem(juce::String(size) + " samples", size + 1);
        }
        if (combo.indexOfItemId(wantedId) < 0) {
            combo.addItem(juce::String(current) + " samples (device)", wantedId);
        }
    }
    combo.setSelectedId(wantedId, juce::dontSendNotification);
}
}

SettingsView::SettingsView(AudioEngine& engine)
    : audioEngine(engine) {
    staged_ = audioEngine.getDeviceSettings();

    titleLabel.setText("Settings", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(titleLabel);

    const auto styleLabel = [this](juce::Label& l) {
        l.setFont(juce::Font(juce::FontOptions(12.0f)));
        l.setColour(juce::Label::textColourId, theme::dim());
        addAndMakeVisible(l);
    };
    styleLabel(clockLabel);
    styleLabel(latencyLabel);
    styleLabel(bufferLabel);
    styleLabel(errorLabel);

    for (auto* label : { &inputBufferSizeLabel, &outputBufferSizeLabel }) {
        label->setFont(juce::Font(juce::FontOptions(12.0f)));
        label->setColour(juce::Label::textColourId, theme::text());
        addAndMakeVisible(*label);
    }
    inputBufferSizeLabel.setText("Input buffer", juce::dontSendNotification);
    outputBufferSizeLabel.setText("Output buffer", juce::dontSendNotification);

    for (auto* combo : { &inputBufferSizeCombo, &outputBufferSizeCombo }) {
        combo->addItem("Auto (driver default)", kAutoBufferItemId);
        for (int s : kBufferSizes) {
            combo->addItem(juce::String(s) + " samples", s + 1);
        }
        combo->setColour(juce::ComboBox::backgroundColourId, theme::control());
        combo->setColour(juce::ComboBox::textColourId, theme::text());
        combo->setColour(juce::ComboBox::arrowColourId, theme::dim());
        combo->setColour(juce::ComboBox::outlineColourId, theme::border());
    }
    inputBufferSizeCombo.onChange = [this] {
        const int id = inputBufferSizeCombo.getSelectedId();
        staged_.inputBuffer = id <= kAutoBufferItemId ? 0 : id - 1;
        updatePendingState();
    };
    outputBufferSizeCombo.onChange = [this] {
        const int id = outputBufferSizeCombo.getSelectedId();
        staged_.outputBuffer = id <= kAutoBufferItemId ? 0 : id - 1;
        updatePendingState();
    };
    addAndMakeVisible(inputBufferSizeCombo);
    addAndMakeVisible(outputBufferSizeCombo);

    measureButton.onClick = [this] {
        if (audioEngine.isLatencyMeasuring()) {
            audioEngine.cancelLatencyMeasurement();
        } else {
            audioEngine.startLatencyMeasurement();
        }
        refresh();
    };
    measureButton.setColour(juce::TextButton::buttonColourId, theme::control());
    measureButton.setColour(juce::TextButton::textColourOffId, theme::text());
    addAndMakeVisible(measureButton);

    measuredLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    measuredLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(measuredLabel);

    tuningLabel.setText("Latency tuning", juce::dontSendNotification);
    tuningLabel.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    tuningLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(tuningLabel);

    const std::array<std::pair<juce::ToggleButton*, AudioEngine::LatencyOption>, 5> tuningToggles {{
        { &lowLatencyInputToggle, AudioEngine::LatencyOption::LowLatencyInput },
        { &exclusiveToggle, AudioEngine::LatencyOption::PreferExclusive },
        { &lowLatencyOutputsToggle, AudioEngine::LatencyOption::LowLatencyOutputs },
        { &minimalClockFifoToggle, AudioEngine::LatencyOption::MinimalClockFifo },
        { &fullDuplexToggle, AudioEngine::LatencyOption::FullDuplexInput },
    }};
    for (const auto& [toggle, option] : tuningToggles) {
        toggle->setColour(juce::ToggleButton::textColourId, theme::text());
        toggle->setColour(juce::ToggleButton::tickColourId, theme::green());
        toggle->setColour(juce::ToggleButton::tickDisabledColourId, theme::dim());
        toggle->onClick = [this, toggle, option] {
            switch (option) {
                case AudioEngine::LatencyOption::LowLatencyInput:
                    staged_.lowLatencyInput = toggle->getToggleState(); break;
                case AudioEngine::LatencyOption::PreferExclusive:
                    staged_.preferExclusive = toggle->getToggleState(); break;
                case AudioEngine::LatencyOption::LowLatencyOutputs:
                    staged_.lowLatencyOutputs = toggle->getToggleState(); break;
                case AudioEngine::LatencyOption::MinimalClockFifo:
                    staged_.minimalClockFifo = toggle->getToggleState(); break;
                case AudioEngine::LatencyOption::FullDuplexInput:
                    staged_.fullDuplexInput = toggle->getToggleState(); break;
            }
            updatePendingState();
        };
        addAndMakeVisible(*toggle);
    }
    lowLatencyInputToggle.setTooltip(
        "Input capture keeps one render block of read delay instead of two. "
        "Saves ~10 ms; may add input underruns on jittery drivers.");
    exclusiveToggle.setTooltip(
        "Try exclusive WASAPI before shared low-latency. Latency depends on the "
        "driver (sometimes higher), and switching reopens every device, so the "
        "audio may glitch briefly.");
    lowLatencyOutputsToggle.setTooltip(
        "Non-clock outputs keep two device buffers instead of four. Saves "
        "~20 ms on those outputs; more sensitive to clock drift. Switching "
        "reopens the outputs (the UI stays responsive).");
    minimalClockFifoToggle.setTooltip(
        "The clock output starts with no primed FIFO. Saves ~10 ms; a missed "
        "render is covered by one block of silence. Switching reopens the "
        "devices.");
    fullDuplexToggle.setTooltip(
        "When the input and the clock output are the same device, use its "
        "hardware clock and one block of input read delay. Saves ~10 ms.");

    pendingLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    pendingLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(pendingLabel);

    for (auto* button : { &applyButton, &revertButton, &reloadAllButton, &reloadDevicesButton }) {
        button->setColour(juce::TextButton::buttonColourId, theme::control());
        button->setColour(juce::TextButton::textColourOffId, theme::text());
        addAndMakeVisible(*button);
    }
    applyButton.onClick = [this] { applyPending(); };
    revertButton.onClick = [this] { revertPending(); };
    reloadAllButton.onClick = [this] {
        audioEngine.reloadAllDevicesAsync();
        refresh();
    };

    devicesLabel.setText("Audio devices", juce::dontSendNotification);
    devicesLabel.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    devicesLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(devicesLabel);

    reloadDevicesViewport.setViewedComponent(&reloadDevicesHolder, false);
    reloadDevicesViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(reloadDevicesViewport);
    reloadDevicesButton.onClick = [this] {
        std::vector<AudioEngine::ReloadableDevice> selected;
        for (const auto& device : reloadDevices_) {
            if (selectedReloadDevices_.contains(device.label)) selected.push_back(device);
        }
        if (!selected.empty()) {
            audioEngine.reloadDevicesAsync(selected);
            refresh();
        }
    };
    reloadDevicesButton.setTooltip(
        "Reopens the checked devices. A device that fails is retried "
        "automatically (up to 3 attempts).");

    startTimer(250);
    refresh();
}

void SettingsView::refresh() {
    if (!dirty_) staged_ = audioEngine.getDeviceSettings();
    clockLabel.setText("Clock: " + audioEngine.getClockDescription(), juce::dontSendNotification);
    const auto report = audioEngine.getLatencyReport();
    if (report.valid) {
        latencyLabel.setText("Latency (est.): " + juce::String((int)std::round(report.totalMs)) + " ms", juce::dontSendNotification);
        bufferLabel.setText("Sample rate: " + juce::String((int)report.sampleRate) + " Hz   Buffer: "
                            + juce::String(report.inputBufferSize) + " in / " + juce::String(report.outputBufferSize) + " out",
                            juce::dontSendNotification);
        if (!inputBufferSizeCombo.isPopupActive()) {
            if (staged_.inputBuffer > 0) showDeviceBuffer(inputBufferSizeCombo, staged_.inputBuffer);
            else inputBufferSizeCombo.setSelectedId(kAutoBufferItemId, juce::dontSendNotification);
        }
        if (!outputBufferSizeCombo.isPopupActive()) {
            if (staged_.outputBuffer > 0) showDeviceBuffer(outputBufferSizeCombo, staged_.outputBuffer);
            else outputBufferSizeCombo.setSelectedId(kAutoBufferItemId, juce::dontSendNotification);
        }
    } else {
        latencyLabel.setText("Latency (est.): n/a", juce::dontSendNotification);
        bufferLabel.setText("Sample rate: n/a", juce::dontSendNotification);
    }

    if (audioEngine.isLatencyMeasuring()) {
        measuredLabel.setText("Measuring round-trip... (connect output to input)", juce::dontSendNotification);
        measureButton.setButtonText("Cancel Measurement");
    } else if (report.measuredValid) {
        measuredLabel.setText("Measured round-trip: " + juce::String((int)std::round(report.measuredMs)) + " ms", juce::dontSendNotification);
        measureButton.setButtonText("Measure Round-Trip Latency");
    } else if (report.measureFailed) {
        measuredLabel.setText("Measurement failed - no loopback detected", juce::dontSendNotification);
        measureButton.setButtonText("Measure Round-Trip Latency");
    } else {
        measuredLabel.setText("Measured round-trip: not measured", juce::dontSendNotification);
        measureButton.setButtonText("Measure Round-Trip Latency");
    }
    juce::String errors;
    for (const auto& c : audioEngine.getChannels()) {
        if (c->inputError.isNotEmpty()) errors += c->name + ": " + c->inputError + "\n";
    }
    for (const auto& o : audioEngine.getOutputs()) {
        if (o->outputError.isNotEmpty()) errors += o->name + ": " + o->outputError + "\n";
    }
    if (report.underruns + report.overruns > 0) errors += "Glitches: " + juce::String((juce::int64)report.overruns) + " overruns / " + juce::String((juce::int64)report.underruns) + " underruns\n";
    const auto fifo = audioEngine.getFifoDiagnostics();
    if (fifo.inputUnderruns > 0 || fifo.inputOverruns > 0) {
        errors += "Input capture: " + juce::String((juce::int64)fifo.inputUnderruns)
            + " underruns / " + juce::String((juce::int64)fifo.inputOverruns)
            + " overruns\n";
    }
    if (fifo.outputLowWatermarks > 0 || fifo.renderMisses > 0) {
        errors += "Output safety: " + juce::String((juce::int64)fifo.outputLowWatermarks)
            + " low-watermark events / " + juce::String((juce::int64)fifo.renderMisses)
            + " render misses\n";
    }
    if (errors.isEmpty()) errors = "No audio errors.";
    errorLabel.setText(errors.trim(), juce::dontSendNotification);

    const bool busy = audioEngine.isOpeningDevices();
    if (!dirty_) {
        lowLatencyInputToggle.setToggleState(staged_.lowLatencyInput, juce::dontSendNotification);
        exclusiveToggle.setToggleState(staged_.preferExclusive, juce::dontSendNotification);
        lowLatencyOutputsToggle.setToggleState(staged_.lowLatencyOutputs, juce::dontSendNotification);
        minimalClockFifoToggle.setToggleState(staged_.minimalClockFifo, juce::dontSendNotification);
        fullDuplexToggle.setToggleState(staged_.fullDuplexInput, juce::dontSendNotification);
    }
    for (auto* toggle : { &lowLatencyInputToggle, &exclusiveToggle, &lowLatencyOutputsToggle,
                           &minimalClockFifoToggle, &fullDuplexToggle }) {
        toggle->setEnabled(!busy);
    }
    inputBufferSizeCombo.setEnabled(!busy);
    outputBufferSizeCombo.setEnabled(!busy);
    reloadAllButton.setEnabled(!busy);
    reloadDevicesViewport.setEnabled(!busy);

    updatePendingState();
    refreshReloadDevices();
}

void SettingsView::updatePendingState() {
    const auto applied = audioEngine.getDeviceSettings();
    dirty_ = staged_.inputBuffer != applied.inputBuffer
          || staged_.outputBuffer != applied.outputBuffer
          || staged_.lowLatencyInput != applied.lowLatencyInput
          || staged_.preferExclusive != applied.preferExclusive
          || staged_.lowLatencyOutputs != applied.lowLatencyOutputs
          || staged_.minimalClockFifo != applied.minimalClockFifo
          || staged_.fullDuplexInput != applied.fullDuplexInput;
    pendingLabel.setText(dirty_ ? "Pending changes - press Apply" : "",
                         juce::dontSendNotification);
    const bool busy = audioEngine.isOpeningDevices();
    applyButton.setEnabled(dirty_ && !busy);
    revertButton.setEnabled(dirty_ && !busy);
}

void SettingsView::applyPending() {
    audioEngine.applyDeviceSettings(staged_);
    dirty_ = false;
    refresh();
}

void SettingsView::revertPending() {
    dirty_ = false;
    refresh();
}

void SettingsView::refreshReloadDevices() {
    const auto devices = audioEngine.getReloadableDevices();
    bool changed = devices.size() != reloadDevices_.size();
    if (!changed) {
        for (std::size_t i = 0; i < devices.size(); ++i) {
            if (devices[i].label != reloadDevices_[i].label) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        juce::StringArray checked = selectedReloadDevices_;
        if (checked.isEmpty() && reloadDevices_.empty() && !devices.empty()) {
            // Keep the previous single-device behaviour on first show.
            checked.add(devices.front().label);
        }

        reloadDevices_ = devices;
        reloadDeviceToggles.clear();
        constexpr int rowHeight = 24;
        for (std::size_t i = 0; i < devices.size(); ++i) {
            auto* toggle = reloadDeviceToggles.add(new juce::ToggleButton(devices[i].label));
            toggle->setColour(juce::ToggleButton::textColourId, theme::text());
            toggle->setColour(juce::ToggleButton::tickColourId, theme::green());
            toggle->setColour(juce::ToggleButton::tickDisabledColourId, theme::dim());
            toggle->setToggleState(checked.contains(devices[i].label),
                                   juce::dontSendNotification);
            toggle->onClick = [this, toggle] {
                selectedReloadDevices_.removeString(toggle->getButtonText());
                if (toggle->getToggleState()) {
                    selectedReloadDevices_.add(toggle->getButtonText());
                }
                refreshReloadDevices();
            };
            reloadDevicesHolder.addAndMakeVisible(toggle);
            toggle->setBounds(0, (int) i * rowHeight, 420, rowHeight);
        }

        juce::StringArray kept;
        for (const auto& label : checked) {
            for (const auto& device : devices) {
                if (device.label == label) {
                    kept.add(label);
                    break;
                }
            }
        }
        selectedReloadDevices_ = kept;
        reloadDevicesHolder.setSize(420, juce::jmax(rowHeight, (int) devices.size() * rowHeight));
    }

    const bool busy = audioEngine.isOpeningDevices();
    reloadDevicesButton.setEnabled(!busy && !selectedReloadDevices_.isEmpty());
    reloadDevicesButton.setButtonText(
        selectedReloadDevices_.isEmpty()
            ? "Reload Selected Devices"
            : "Reload Selected Devices (" + juce::String(selectedReloadDevices_.size()) + ")");
}

void SettingsView::timerCallback() { refresh(); }

void SettingsView::resized() {
    auto area = getLocalBounds().reduced(24);
    titleLabel.setBounds(area.removeFromTop(30));
    area.removeFromTop(8);
    clockLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);
    latencyLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);
    bufferLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(8);
    errorLabel.setBounds(area.removeFromTop(60));
    area.removeFromTop(12);

    auto row = area.removeFromTop(24);
    inputBufferSizeLabel.setBounds(row.removeFromLeft(100));
    row.removeFromLeft(8);
    inputBufferSizeCombo.setBounds(row.removeFromLeft(160));
    row.removeFromLeft(16);
    outputBufferSizeLabel.setBounds(row.removeFromLeft(105));
    row.removeFromLeft(8);
    outputBufferSizeCombo.setBounds(row.removeFromLeft(160));

    area.removeFromTop(8);
    auto mrow = area.removeFromTop(24);
    measureButton.setBounds(mrow.removeFromLeft(220));
    mrow.removeFromLeft(16); // keep the result clear of the button
    measuredLabel.setBounds(mrow);

    area.removeFromTop(18);
    tuningLabel.setBounds(area.removeFromTop(24));
    area.removeFromTop(6);
    for (auto* toggle : { &lowLatencyInputToggle, &exclusiveToggle, &lowLatencyOutputsToggle,
                           &minimalClockFifoToggle, &fullDuplexToggle }) {
        toggle->setBounds(area.removeFromTop(24).removeFromLeft(430));
        area.removeFromTop(4);
    }

    area.removeFromTop(14);
    auto pendingRow = area.removeFromTop(24);
    pendingLabel.setBounds(pendingRow.removeFromLeft(230));
    applyButton.setBounds(pendingRow.removeFromLeft(90));
    pendingRow.removeFromLeft(8);
    revertButton.setBounds(pendingRow.removeFromLeft(90));

    area.removeFromTop(18);
    devicesLabel.setBounds(area.removeFromTop(24));
    area.removeFromTop(6);
    reloadAllButton.setBounds(area.removeFromTop(24).removeFromLeft(220));
    area.removeFromTop(6);
    auto listRow = area.removeFromTop(96);
    reloadDevicesViewport.setBounds(listRow.removeFromLeft(430));
    listRow.removeFromLeft(8);
    reloadDevicesButton.setBounds(listRow.removeFromTop(24).removeFromLeft(230));
}