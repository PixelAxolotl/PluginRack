#include "SettingsView.h"

#include <algorithm>
#include <array>
#include <utility>

namespace {
constexpr int kBufferSizes[] = { 32, 64, 128, 256, 480, 512, 1024, 2048, 4096 };
constexpr int kAutoBufferItemId = 1; // ComboBox ids must be non-zero.

// Auto-reload pacing: a broken device gets three rounds five seconds apart,
// then it stays down until the user reloads it or the error clears.
constexpr std::uint32_t kAutoReloadCooldownMs = 5000;
constexpr int kMaxAutoReloadRounds = 3;

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

    addAndMakeVisible(header);

    const auto styleLabel = [this](juce::Label& l) {
        l.setFont(theme::ui::bodyFont());
        l.setColour(juce::Label::textColourId, theme::dim());
        addAndMakeVisible(l);
    };
    styleLabel(clockLabel);
    styleLabel(latencyLabel);
    styleLabel(bufferLabel);
    styleLabel(errorLabel);

    for (auto* label : { &inputBufferSizeLabel, &outputBufferSizeLabel }) {
        label->setFont(theme::ui::bodyFont());
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
        theme::ui::styleCombo(*combo);
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

    roundTripLabel.setText("ROUND-TRIP LATENCY", juce::dontSendNotification);
    roundTripLabel.setFont(theme::ui::sectionFont());
    roundTripLabel.setColour(juce::Label::textColourId, theme::dim().withAlpha(0.8f));
    addAndMakeVisible(roundTripLabel);
    theme::ui::styleBetaLabel(roundTripBeta);
    addAndMakeVisible(roundTripBeta);

    measuredLabel.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    measuredLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(measuredLabel);

    measuredDetailLabel.setFont(theme::ui::captionFont());
    measuredDetailLabel.setColour(juce::Label::textColourId, theme::dim());
    measuredDetailLabel.setMinimumHorizontalScale(0.7f);
    addAndMakeVisible(measuredDetailLabel);

    listenLabel.setFont(theme::ui::bodyFont());
    listenLabel.setColour(juce::Label::textColourId, theme::dim());
    listenLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(listenLabel);

    theme::ui::styleCombo(listenCombo);
    listenCombo.setTooltip("Which open input listens for the click coming back");
    listenCombo.onChange = [this] {
        listenSelection_ = listenCombo.getSelectedId() <= 1 ? juce::String()
                                                            : listenCombo.getText();
        refresh();
    };
    addAndMakeVisible(listenCombo);

    theme::ui::styleButton(measureButton);
    measureButton.setTooltip(
        "Sends one click out of the clock output and measures how long it takes "
        "to come back on the selected input. This needs a cable from that output "
        "to that input.");
    measureButton.onClick = [this] {
        if (audioEngine.isLatencyMeasuring()) audioEngine.cancelLatencyMeasurement();
        else audioEngine.startLatencyMeasurement(listenSelection_);
        refresh();
    };
    addAndMakeVisible(measureButton);

    tuningLabel.setText("LATENCY TUNING (OPT-IN)", juce::dontSendNotification);
    tuningLabel.setFont(theme::ui::sectionFont());
    tuningLabel.setColour(juce::Label::textColourId, theme::dim().withAlpha(0.8f));
    addAndMakeVisible(tuningLabel);
    theme::ui::styleBetaLabel(tuningBeta);
    addAndMakeVisible(tuningBeta);

    const std::array<std::pair<juce::ToggleButton*, AudioEngine::LatencyOption>, 5> tuningToggles {{
        { &lowLatencyInputToggle, AudioEngine::LatencyOption::LowLatencyInput },
        { &exclusiveToggle, AudioEngine::LatencyOption::PreferExclusive },
        { &lowLatencyOutputsToggle, AudioEngine::LatencyOption::LowLatencyOutputs },
        { &minimalClockFifoToggle, AudioEngine::LatencyOption::MinimalClockFifo },
        { &fullDuplexToggle, AudioEngine::LatencyOption::FullDuplexInput },
    }};
    for (const auto& [toggle, option] : tuningToggles) {
        theme::ui::styleToggle(*toggle);
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
            staged_.latencyTuningOptIn = staged_.lowLatencyInput
                                      || staged_.preferExclusive
                                      || staged_.lowLatencyOutputs
                                      || staged_.minimalClockFifo
                                      || staged_.fullDuplexInput;
            updatePendingState();
        };
        addAndMakeVisible(*toggle);
    }
    lowLatencyInputToggle.setTooltip(
        "Input capture keeps one render block of read delay instead of two. "
        "Saves ~10 ms while stable; it adds a block automatically after a real "
        "starvation on a jittery driver.");
    exclusiveToggle.setTooltip(
        "Try exclusive WASAPI for the standalone clock output. Shared WASAPI "
        "remains first for other streams and for same-device input, because "
        "exclusive clients cannot share an endpoint reliably.");
    lowLatencyOutputsToggle.setTooltip(
        "Non-clock outputs keep two device buffers instead of four. Saves "
        "~20 ms on those outputs; more sensitive to clock drift. Switching "
        "reopens the outputs (the UI stays responsive).");
    minimalClockFifoToggle.setTooltip(
        "The clock output starts with no primed FIFO. Saves ~10 ms; a missed "
        "render is covered by one block of silence. Switching reopens the "
        "devices.");
    fullDuplexToggle.setTooltip(
        "When the input and clock output use the same endpoint, start capture "
        "with one block of read delay. It is not a shared callback; the delay "
        "grows automatically if the device starves.");

    pendingLabel.setFont(theme::ui::captionFont());
    pendingLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(pendingLabel);

    for (auto* button : { &applyButton, &revertButton, &reloadAllButton }) {
        theme::ui::styleButton(*button);
        addAndMakeVisible(*button);
    }
    applyButton.onClick = [this] { applyPending(); };
    revertButton.onClick = [this] { revertPending(); };
    reloadAllButton.onClick = [this] {
        audioEngine.reloadAllDevicesAsync();
        refresh();
    };

    devicesLabel.setText("AUDIO DEVICES", juce::dontSendNotification);
    devicesLabel.setFont(theme::ui::sectionFont());
    devicesLabel.setColour(juce::Label::textColourId, theme::dim().withAlpha(0.8f));
    addAndMakeVisible(devicesLabel);

    reloadDevicesViewport.setViewedComponent(&reloadDevicesHolder, false);
    reloadDevicesViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(reloadDevicesViewport);
    theme::ui::styleToggle(autoReloadToggle);
    autoReloadToggle.setTooltip(
        "Reopens a checked device by itself when it reports an error. Three "
        "rounds, five seconds apart, then it stays off until you reload it.");
    autoReloadToggle.onClick = [this] {
        audioEngine.setAutoReloadDevices(autoReloadToggle.getToggleState());
        if (autoReloadToggle.getToggleState()) pushWatchedDevices();
        refresh();
    };
    addAndMakeVisible(autoReloadToggle);

    rackThumbnailsLabel.setFont(theme::ui::bodyFont());
    rackThumbnailsLabel.setColour(juce::Label::textColourId, theme::text());
    rackThumbnailsLabel.setTooltip(
        "How often the Rack view redraws the snapshot of the plugin editor it "
        "shows in each insert card.");
    addAndMakeVisible(rackThumbnailsLabel);

    theme::ui::styleCombo(rackThumbnailFpsCombo);
    for (const int fps : { 5, 10, 15, 20, 30 })
        rackThumbnailFpsCombo.addItem(juce::String(fps) + " fps", fps);
    rackThumbnailFpsCombo.setTooltip(
        "Thumbnail refresh rate. Lower costs less CPU when a heavy plugin "
        "editor is open.");
    rackThumbnailFpsCombo.onChange = [this] {
        audioEngine.setRackThumbnailFps(rackThumbnailFpsCombo.getSelectedId());
    };
    addAndMakeVisible(rackThumbnailFpsCombo);

    theme::ui::styleToggle(closeToTrayToggle);
    closeToTrayToggle.setTooltip("Keep PluginRack running in the system tray when the window is closed.");
    closeToTrayToggle.onClick = [this] {
        if (onCloseToTrayChanged) onCloseToTrayChanged(closeToTrayToggle.getToggleState());
    };
    addAndMakeVisible(closeToTrayToggle);

    startTimer(250);
    refresh();
}

void SettingsView::setCloseToTrayEnabled(bool enabled) {
    closeToTrayToggle.setToggleState(enabled, juce::dontSendNotification);
}

void SettingsView::refresh() {
    audioEngine.pollLatencyMeasurement();
    if (!dirty_) staged_ = audioEngine.getDeviceSettings();
    clockLabel.setText("Clock: " + audioEngine.getClockDescription(), juce::dontSendNotification);
    const auto report = audioEngine.getLatencyReport();
    if (report.valid) {
        latencyLabel.setText("Latency (est.): " + juce::String((int)std::round(report.totalMs)) + " ms", juce::dontSendNotification);
        bufferLabel.setText("Sample rate: " + juce::String((int)report.sampleRate) + " Hz   Buffer: "
                            + juce::String(report.inputBufferSize) + " in / " + juce::String(report.outputBufferSize) + " out",
                            juce::dontSendNotification);
    } else {
        latencyLabel.setText("Latency (est.): n/a", juce::dontSendNotification);
        bufferLabel.setText("Sample rate: n/a", juce::dontSendNotification);
    }

    // The staged choice is shown whether or not a device is open, so the page
    // never presents an empty field as if nothing were configured.
    if (!inputBufferSizeCombo.isPopupActive()) {
        if (staged_.inputBuffer > 0) showDeviceBuffer(inputBufferSizeCombo, staged_.inputBuffer);
        else inputBufferSizeCombo.setSelectedId(kAutoBufferItemId, juce::dontSendNotification);
    }
    if (!outputBufferSizeCombo.isPopupActive()) {
        if (staged_.outputBuffer > 0) showDeviceBuffer(outputBufferSizeCombo, staged_.outputBuffer);
        else outputBufferSizeCombo.setSelectedId(kAutoBufferItemId, juce::dontSendNotification);
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
    const int thumbFps = audioEngine.getRackThumbnailFps();
    if (!rackThumbnailFpsCombo.isPopupActive()) {
        // A scene can carry a rate that is not one of the presets; show it
        // rather than an empty field.
        if (rackThumbnailFpsCombo.indexOfItemId(thumbFps) < 0)
            rackThumbnailFpsCombo.addItem(juce::String(thumbFps) + " fps", thumbFps);
        rackThumbnailFpsCombo.setSelectedId(thumbFps, juce::dontSendNotification);
    }
    reloadAllButton.setEnabled(!busy);
    reloadDevicesViewport.setEnabled(!busy);
    updatePendingState();
    refreshReloadDevices();
    refreshListenChoices();
    updateMeasurement(report);
    pollAutoReload();
}

void SettingsView::updatePendingState() {
    const auto applied = audioEngine.getDeviceSettings();
    dirty_ = staged_.inputBuffer != applied.inputBuffer
          || staged_.outputBuffer != applied.outputBuffer
          || staged_.latencyTuningOptIn != applied.latencyTuningOptIn
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

void SettingsView::refreshListenChoices() {
    juce::StringArray names;
    for (const auto& channel : audioEngine.getChannels()) {
        if (channel->input != nullptr && channel->inputDeviceName.isNotEmpty())
            names.addIfNotAlreadyThere(channel->inputDeviceName);
    }
    if (listenChoicesBuilt_ && names == listenNames_) return;
    listenChoicesBuilt_ = true;
    listenNames_ = names;

    listenCombo.clear(juce::dontSendNotification);
    listenCombo.addItem("Any open input", 1);
    int selectedId = 1;
    for (int i = 0; i < names.size(); ++i) {
        listenCombo.addItem(names[i], i + 2);
        if (names[i] == listenSelection_) selectedId = i + 2;
    }
    listenCombo.setSelectedId(selectedId, juce::dontSendNotification);
    if (selectedId == 1) listenSelection_.clear();
}

juce::String SettingsView::listenTarget() const {
    return listenSelection_.isEmpty() ? juce::String("an open input") : listenSelection_;
}

void SettingsView::updateMeasurement(const AudioEngine::LatencyReport& report) {
    const bool measuring = audioEngine.isLatencyMeasuring();
    const bool busy = audioEngine.isOpeningDevices();
    const auto clockDevice = audioEngine.getClockOutputDeviceName();
    bool hasCapture = false;
    for (const auto& channel : audioEngine.getChannels()) {
        if (channel->input != nullptr) {
            hasCapture = true;
            break;
        }
    }
    const bool ready = clockDevice.isNotEmpty() && hasCapture;
    const auto estimate = report.valid
                              ? juce::String((int)std::round(report.totalMs)) + " ms"
                              : juce::String();
    const auto target = listenTarget();

    measureButton.setButtonText(measuring ? "Cancel Measurement" : "Measure Round-Trip");
    measureButton.setEnabled(!busy && (measuring || ready));

    const auto setDetail = [this](const juce::String& text, juce::Colour colour) {
        measuredDetailLabel.setText(text, juce::dontSendNotification);
        measuredDetailLabel.setColour(juce::Label::textColourId, colour);
    };

    if (busy) {
        measuredLabel.setText("Starting", juce::dontSendNotification);
        measuredLabel.setColour(juce::Label::textColourId, theme::dim());
        setDetail("Audio devices are starting up.", theme::dim());
    } else if (measuring) {
        measuredLabel.setText("Measuring", juce::dontSendNotification);
        measuredLabel.setColour(juce::Label::textColourId, theme::amber());
        setDetail("Click sent out of " + clockDevice + ". Waiting for it on " + target + ".",
                  theme::dim());
    } else if (report.measuredValid) {
        measuredLabel.setText(juce::String((int)std::round(report.measuredMs)) + " ms",
                              juce::dontSendNotification);
        measuredLabel.setColour(juce::Label::textColourId, theme::green());
        setDetail(estimate.isEmpty()
                      ? "Round trip through " + target
                      : "Round trip through " + target + "  -  estimated " + estimate,
                  theme::dim());
    } else if (report.measureFailed) {
        measuredLabel.setText("No return", juce::dontSendNotification);
        measuredLabel.setColour(juce::Label::textColourId, theme::red());
        setDetail("No click came back on " + target + ". Check the cable from " + clockDevice + ".",
                  theme::red());
    } else if (!ready) {
        measuredLabel.setText("Not measured", juce::dontSendNotification);
        measuredLabel.setColour(juce::Label::textColourId, theme::text());
        setDetail(clockDevice.isEmpty()
                      ? "Assign an output device first - the click leaves through the clock output."
                      : "Assign an input device first - the click has to come back through it.",
                  theme::amber());
    } else {
        measuredLabel.setText("Not measured", juce::dontSendNotification);
        measuredLabel.setColour(juce::Label::textColourId, theme::text());
        setDetail("Cable " + clockDevice + " into " + target
                      + ", then measure: the click leaves that output and comes back through that input.",
                  theme::dim());
    }
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
        reloadDevices_ = devices;
        reloadDeviceToggles.clear();
        constexpr int rowHeight = 24;
        for (std::size_t i = 0; i < devices.size(); ++i) {
            auto* toggle = reloadDeviceToggles.add(new juce::ToggleButton(devices[i].label));
            theme::ui::styleToggle(*toggle);
            toggle->onClick = [this, toggle] {
                selectedReloadDevices_.removeString(toggle->getButtonText());
                if (toggle->getToggleState()) {
                    selectedReloadDevices_.add(toggle->getButtonText());
                }
                pushWatchedDevices();
                refreshReloadDevices();
            };
            reloadDevicesHolder.addAndMakeVisible(toggle);
            toggle->setBounds(0, (int) i * rowHeight, 420, rowHeight);
        }
        reloadDevicesHolder.setSize(420, juce::jmax(rowHeight, (int) devices.size() * rowHeight));
    }

    syncWatchedFromEngine();
    // The list may have appeared or gone away; its bounds depend on that.
    if (changed) resized();
}

void SettingsView::pushWatchedDevices() {
    std::vector<pluginrack::WatchedDevice> watched;
    for (const auto& device : reloadDevices_) {
        if (selectedReloadDevices_.contains(device.label))
            watched.push_back({device.isInput, device.stripId});
    }
    audioEngine.setWatchedDevices(watched);
}

void SettingsView::syncWatchedFromEngine() {
    // The engine owns the selection: a scene load replaces it, and the checks
    // here follow. Strip ids (not labels) are the identity, so a rename does
    // not drop a device from the watch list.
    const auto watched = audioEngine.getWatchedDevices();
    juce::StringArray labels;
    for (const auto& device : reloadDevices_) {
        for (const auto& entry : watched) {
            if (entry.isInput == device.isInput && entry.stripId == device.stripId) {
                labels.add(device.label);
                break;
            }
        }
    }

    if (labels != selectedReloadDevices_) {
        selectedReloadDevices_ = labels;
        for (auto* toggle : reloadDeviceToggles)
            toggle->setToggleState(selectedReloadDevices_.contains(toggle->getButtonText()),
                                   juce::dontSendNotification);
    }

    autoReloadToggle.setToggleState(audioEngine.getAutoReloadDevices(),
                                    juce::dontSendNotification);
    autoReloadToggle.setEnabled(!audioEngine.isOpeningDevices());
}

void SettingsView::pollAutoReload() {
    if (!audioEngine.getAutoReloadDevices() || audioEngine.isOpeningDevices()) return;

    const auto watched = audioEngine.getWatchedDevices();
    const auto now = juce::Time::getMillisecondCounter();
    for (const auto& device : reloadDevices_) {
        const std::pair<bool, std::uint32_t> key{device.isInput, device.stripId};
        const bool isWatched = std::any_of(watched.begin(), watched.end(),
                                           [&device](const pluginrack::WatchedDevice& entry) {
                                               return entry.isInput == device.isInput
                                                      && entry.stripId == device.stripId;
                                           });
        if (!isWatched) continue;

        bool failing = false;
        if (device.isInput) {
            if (auto* channel = audioEngine.getChannelById(device.stripId))
                failing = channel->inputError.isNotEmpty();
        } else if (auto* output = audioEngine.getOutputById(device.stripId)) {
            failing = output->outputError.isNotEmpty();
        }

        auto& state = autoReloadState_[key];
        if (!failing) {
            state = {0, 0}; // healthy again: the next error starts over
            continue;
        }
        if (state.second >= kMaxAutoReloadRounds) continue;
        if (state.first != 0 && now - state.first < kAutoReloadCooldownMs) continue;
        // One reopen at a time: the rest wait for the next tick instead of
        // spending an attempt on a job the engine would refuse.
        if (audioEngine.isOpeningDevices()) break;

        state.first = now;
        ++state.second;
        audioEngine.reloadDevicesAsync({device});
    }
}

void SettingsView::timerCallback() { refresh(); }

void SettingsView::resized() {
    auto area = getLocalBounds().reduced(theme::ui::kPagePad);
    header.setBounds(area.removeFromTop(theme::ui::kHeaderH));
    area.removeFromTop(theme::ui::kHeaderGap);

    // Caption plus its BETA tag, which follows the caption's own text width so
    // the two never overlap as the words change.
    const auto placeCaption = [](juce::Rectangle<int> row, juce::Label& caption,
                                 juce::Label& badge) {
        caption.setBounds(row);
        const int textWidth = juce::GlyphArrangement::getStringWidthInt(caption.getFont(),
                                                                       caption.getText());
        badge.setBounds(row.getX() + textWidth + 8, row.getY(), 44, row.getHeight());
    };

    const int lineH = 20;
    clockLabel.setBounds(area.removeFromTop(lineH));
    area.removeFromTop(2);
    latencyLabel.setBounds(area.removeFromTop(lineH));
    area.removeFromTop(2);
    bufferLabel.setBounds(area.removeFromTop(lineH));
    area.removeFromTop(theme::ui::kRowGap);
    errorLabel.setBounds(area.removeFromTop(60));
    area.removeFromTop(theme::ui::kRowGap);

    auto fields = area.removeFromTop(theme::ui::kControlH);
    inputBufferSizeLabel.setBounds(fields.removeFromLeft(92));
    fields.removeFromLeft(theme::ui::kRowGap);
    inputBufferSizeCombo.setBounds(fields.removeFromLeft(160));
    fields.removeFromLeft(theme::ui::kSectionGap);
    outputBufferSizeLabel.setBounds(fields.removeFromLeft(96));
    fields.removeFromLeft(theme::ui::kRowGap);
    outputBufferSizeCombo.setBounds(fields.removeFromLeft(160));

    area.removeFromTop(theme::ui::kRowGap);
    auto pendingRow = area.removeFromTop(theme::ui::kControlH);
    applyButton.setBounds(pendingRow.removeFromLeft(90));
    pendingRow.removeFromLeft(theme::ui::kRowGap);
    revertButton.setBounds(pendingRow.removeFromLeft(90));
    pendingRow.removeFromLeft(theme::ui::kSectionGap);
    pendingLabel.setBounds(pendingRow);

    area.removeFromTop(theme::ui::kSectionGap);
    placeCaption(area.removeFromTop(theme::ui::kSectionH), roundTripLabel, roundTripBeta);
    area.removeFromTop(theme::ui::kRowGap);
    auto measureRow = area.removeFromTop(28);
    measureButton.setBounds(measureRow.removeFromRight(theme::ui::kMeasureW)
                                .withSizeKeepingCentre(theme::ui::kMeasureW,
                                                       theme::ui::kControlH));
    measureRow.removeFromRight(10);
    listenCombo.setBounds(measureRow.removeFromRight(220)
                              .withSizeKeepingCentre(220, theme::ui::kControlH));
    measureRow.removeFromRight(theme::ui::kRowGap);
    listenLabel.setBounds(measureRow.removeFromRight(64));
    measureRow.removeFromRight(12);
    measuredLabel.setBounds(measureRow);
    area.removeFromTop(2);
    measuredDetailLabel.setBounds(area.removeFromTop(16));

    area.removeFromTop(theme::ui::kSectionGap);
    placeCaption(area.removeFromTop(theme::ui::kSectionH), tuningLabel, tuningBeta);
    area.removeFromTop(theme::ui::kRowGap);
    for (auto* toggle : { &lowLatencyInputToggle, &exclusiveToggle, &lowLatencyOutputsToggle,
                           &minimalClockFifoToggle, &fullDuplexToggle }) {
        toggle->setBounds(area.removeFromTop(24).removeFromLeft(430));
        area.removeFromTop(4);
    }

    area.removeFromTop(theme::ui::kRowGap);
    auto thumbRow = area.removeFromTop(theme::ui::kControlH);
    rackThumbnailsLabel.setBounds(thumbRow.removeFromLeft(140));
    thumbRow.removeFromLeft(theme::ui::kRowGap);
    rackThumbnailFpsCombo.setBounds(thumbRow.removeFromLeft(90));
    thumbRow.removeFromLeft(theme::ui::kSectionGap);
    closeToTrayToggle.setBounds(thumbRow.removeFromLeft(320));

    area.removeFromTop(theme::ui::kSectionGap);
    devicesLabel.setBounds(area.removeFromTop(theme::ui::kSectionH));
    area.removeFromTop(theme::ui::kRowGap);
    reloadAllButton.setBounds(area.removeFromTop(theme::ui::kControlH).removeFromLeft(220));

    // The list only exists when at least one device is open; without it the
    // auto-reload switch sits directly under the reload action.
    area.removeFromTop(theme::ui::kRowGap);
    const bool hasDevices = !reloadDevices_.empty();
    reloadDevicesViewport.setVisible(hasDevices);
    reloadDevicesViewport.setBounds(hasDevices
                                        ? area.removeFromTop(96).removeFromLeft(430)
                                        : juce::Rectangle<int>());
    area.removeFromTop(theme::ui::kRowGap);
    autoReloadToggle.setBounds(area.removeFromTop(theme::ui::kControlH).removeFromLeft(380));
}
