#include "SettingsView.h"

#include <algorithm>
#include <array>
#include <utility>

#include "LevelMeter.h"

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
    addAndMakeVisible(pendingLabel);
    addAndMakeVisible(applyButton);
    addAndMakeVisible(revertButton);
    viewport.setViewedComponent(&body, false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    const auto add = [this](juce::Component& component) { body.addAndMakeVisible(component); };
    const auto styleCaption = [this, add](juce::Label& label) {
        label.setFont(theme::ui::sectionFont());
        label.setColour(juce::Label::textColourId, theme::dim());
        add(label);
    };
    const auto styleStatCaption = [this](juce::Label& label) {
        label.setFont(theme::ui::sectionFont());
        label.setColour(juce::Label::textColourId, theme::dim());
        label.setJustificationType(juce::Justification::centredLeft);
        body.addAndMakeVisible(label);
    };
    const auto styleStatValue = [this](juce::Label& label) {
        label.setFont(theme::ui::bodyBoldFont());
        label.setColour(juce::Label::textColourId, theme::text());
        label.setJustificationType(juce::Justification::centredLeft);
        label.setMinimumHorizontalScale(0.7f);
        body.addAndMakeVisible(label);
    };

    // AUDIO STATUS: the numbers that describe the running engine, then the
    // sentence that explains what is wrong with it.
    styleCaption(statusCaption);
    styleStatCaption(clockStatCaption);
    styleStatValue(clockStatValue);
    styleStatCaption(latencyStatCaption);
    styleStatValue(latencyStatValue);
    styleStatCaption(bufferStatCaption);
    styleStatValue(bufferStatValue);
    issuesText.setText("Waiting for audio", theme::dim());
    add(issuesText);

    styleCaption(bufferCaption);
    for (auto* label : { &inputBufferSizeLabel, &outputBufferSizeLabel }) {
        label->setFont(theme::ui::bodyFont());
        label->setColour(juce::Label::textColourId, theme::text());
        add(*label);
    }
    inputBufferSizeLabel.setText("Input buffer", juce::dontSendNotification);
    outputBufferSizeLabel.setText("Output buffer", juce::dontSendNotification);

    for (auto* combo : { &inputBufferSizeCombo, &outputBufferSizeCombo }) {
        combo->addItem("Auto (driver default)", kAutoBufferItemId);
        for (int s : kBufferSizes) {
            combo->addItem(juce::String(s) + " samples", s + 1);
        }
        theme::ui::styleCombo(*combo);
        // The wheel is the page's scroll gesture here; it must not change a
        // setting when the pointer happens to rest over a field.
        combo->setScrollWheelEnabled(false);
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
    add(inputBufferSizeCombo);
    add(outputBufferSizeCombo);

    // Staged-changes bar: pinned in the page header so it stays visible
    // while the Buffer Size and Latency Tuning sections scroll. The combos
    // above and the toggles below share this one Apply step.
    pendingLabel.setFont(theme::ui::captionFont());
    pendingLabel.setColour(juce::Label::textColourId, theme::dim());
    pendingLabel.setJustificationType(juce::Justification::centredRight);

    for (auto* button : { &applyButton, &revertButton }) {
        theme::ui::styleButton(*button);
    }
    applyButton.onClick = [this] { applyPending(); };
    revertButton.onClick = [this] { revertPending(); };

    roundTripLabel.setText("ROUND-TRIP LATENCY", juce::dontSendNotification);
    styleCaption(roundTripLabel);
    theme::ui::styleBetaLabel(roundTripBeta);
    add(roundTripBeta);

    measuredLabel.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    measuredLabel.setColour(juce::Label::textColourId, theme::text());
    add(measuredLabel);

    measuredDetailLabel.setFont(theme::ui::captionFont());
    measuredDetailLabel.setColour(juce::Label::textColourId, theme::dim());
    measuredDetailLabel.setMinimumHorizontalScale(0.7f);
    add(measuredDetailLabel);
    add(budgetView);
    renderLoadLabel.setFont(theme::ui::captionFont());
    renderLoadLabel.setColour(juce::Label::textColourId, theme::dim());
    add(renderLoadLabel);

    listenLabel.setFont(theme::ui::bodyFont());
    listenLabel.setColour(juce::Label::textColourId, theme::dim());
    listenLabel.setJustificationType(juce::Justification::centredRight);
    add(listenLabel);

    theme::ui::styleCombo(listenCombo);
    listenCombo.setTooltip("Which open input listens for the click coming back");
    listenCombo.onChange = [this] {
        listenSelection_ = listenCombo.getSelectedId() <= 1 ? juce::String()
                                                            : listenCombo.getText();
        refresh();
    };
    add(listenCombo);

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
    add(measureButton);

    tuningLabel.setText("LATENCY TUNING (OPT-IN)", juce::dontSendNotification);
    styleCaption(tuningLabel);
    theme::ui::styleBetaLabel(tuningBeta);
    add(tuningBeta);

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
        add(*toggle);
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
        "When the input and the clock output use the same endpoint, capture and "
        "render share one driver callback: no capture ring, no clock FIFO. If the "
        "endpoint refuses to open both directions, the app falls back to a "
        "separate capture automatically.");

    theme::ui::styleButton(profileButton);
    profileButton.setTooltip(
        "Steps the buffer down one size at a time, watches for real glitches, "
        "and goes back to the size you started from on the first failure. "
        "Takes about a minute per step.");
    profileButton.onClick = [this] {
        if (audioEngine.isLatencyProfileActive()) {
            audioEngine.cancelLatencyProfile();
        } else {
            audioEngine.startLatencyProfile();
        }
        refresh();
    };
    add(profileButton);
    add(profileStatusLabel);

    theme::ui::styleButton(forceAsioButton);
    forceAsioButton.setTooltip(
        "Moves the clock output onto the ASIO driver for the same interface. "
        "ASIO bypasses the Windows audio engine, so it reaches smaller buffers "
        "and reports its real latency.");
    forceAsioButton.onClick = [this] {
        forceAsioStatus_ = audioEngine.forceAsio();
        refresh();
    };
    add(forceAsioButton);
    add(asioStatusLabel);

    // APP
    styleCaption(appCaption);

    rackThumbnailsLabel.setFont(theme::ui::bodyFont());
    rackThumbnailsLabel.setColour(juce::Label::textColourId, theme::text());
    rackThumbnailsLabel.setTooltip(
        "How often the Rack view redraws the snapshot of the plugin editor it "
        "shows in each insert card.");
    add(rackThumbnailsLabel);

    theme::ui::styleCombo(rackThumbnailFpsCombo);
    for (const int fps : { 5, 10, 15, 20, 30 })
        rackThumbnailFpsCombo.addItem(juce::String(fps) + " fps", fps);
    rackThumbnailFpsCombo.setTooltip(
        "Thumbnail refresh rate. Lower costs less CPU when a heavy plugin "
        "editor is open.");
    rackThumbnailFpsCombo.setScrollWheelEnabled(false);
    rackThumbnailFpsCombo.onChange = [this] {
        audioEngine.setRackThumbnailFps(rackThumbnailFpsCombo.getSelectedId());
    };
    add(rackThumbnailFpsCombo);

    rackPreviewModeLabel.setFont(theme::ui::bodyFont());
    rackPreviewModeLabel.setColour(juce::Label::textColourId, theme::text());
    rackPreviewModeLabel.setTooltip(
        "Which insert cards keep a live preview. Selected only is cheapest; "
        "rotating refreshes background cards one at a time; all live keeps "
        "every card moving and costs the most CPU.");
    add(rackPreviewModeLabel);

    theme::ui::styleCombo(rackPreviewModeCombo);
    rackPreviewModeCombo.addItem("Selected only", 1);
    rackPreviewModeCombo.addItem("Rotate background", 2);
    rackPreviewModeCombo.addItem("All live", 3);
    rackPreviewModeCombo.setTooltip(
        "Background preview mode. Rotate and All live cost extra CPU.");
    rackPreviewModeCombo.setScrollWheelEnabled(false);
    rackPreviewModeCombo.onChange = [this] {
        using pluginrack::RackPreviewMode;
        const int id = rackPreviewModeCombo.getSelectedId();
        audioEngine.setRackPreviewMode(id == 3 ? RackPreviewMode::AllLive
                                     : id == 2 ? RackPreviewMode::Rotate
                                               : RackPreviewMode::Selected);
    };
    add(rackPreviewModeCombo);

    peakHoldLabel.setFont(theme::ui::bodyFont());
    peakHoldLabel.setColour(juce::Label::textColourId, theme::text());
    peakHoldLabel.setTooltip("How long every level meter keeps its peak line before it falls back.");
    add(peakHoldLabel);

    theme::ui::styleCombo(peakHoldCombo);
    for (const int ms : LevelMeter::kPeakHoldChoices)
        peakHoldCombo.addItem(LevelMeter::peakHoldLabel(ms), ms);
    peakHoldCombo.setTooltip("Peak-hold interval for the channel, output and mixer meters.");
    peakHoldCombo.setScrollWheelEnabled(false);
    peakHoldCombo.onChange = [this] {
        const int ms = peakHoldCombo.getSelectedId();
        if (ms > 0) LevelMeter::setPeakHoldMs(ms);
    };
    add(peakHoldCombo);

    theme::ui::styleToggle(closeToTrayToggle);
    closeToTrayToggle.setTooltip("Keep PluginRack running in the system tray when the window is closed.");
    closeToTrayToggle.onClick = [this] {
        if (onCloseToTrayChanged) onCloseToTrayChanged(closeToTrayToggle.getToggleState());
    };
    add(closeToTrayToggle);

    // AUDIO DEVICES
    devicesLabel.setText("AUDIO DEVICES", juce::dontSendNotification);
    styleCaption(devicesLabel);

    theme::ui::styleButton(reloadAllButton);
    add(reloadAllButton);
    reloadAllButton.onClick = [this] {
        audioEngine.reloadAllDevicesAsync();
        refresh();
    };

    reloadDevicesViewport.setViewedComponent(&reloadDevicesHolder, false);
    reloadDevicesViewport.setScrollBarsShown(true, false);
    add(reloadDevicesViewport);
    theme::ui::styleToggle(autoReloadToggle);
    autoReloadToggle.setTooltip(
        "Reopens a checked device by itself when it reports an error. Three "
        "rounds, five seconds apart, then it stays off until you reload it.");
    autoReloadToggle.onClick = [this] {
        audioEngine.setAutoReloadDevices(autoReloadToggle.getToggleState());
        if (autoReloadToggle.getToggleState()) pushWatchedDevices();
        refresh();
    };
    add(autoReloadToggle);

    startTimer(250);
    refresh();
}

void SettingsView::setCloseToTrayEnabled(bool enabled) {
    closeToTrayToggle.setToggleState(enabled, juce::dontSendNotification);
}

void SettingsView::refresh() {
    audioEngine.pollLatencyMeasurement();
    if (!dirty_) staged_ = audioEngine.getDeviceSettings();
    bool relayout = false;

    const auto report = audioEngine.getLatencyReport();
    if (hasBudget_ != report.valid) {
        hasBudget_ = report.valid;
        relayout = true;
    }
    const auto clockDescription = audioEngine.getClockDescription();
    clockStatValue.setText(clockDescription, juce::dontSendNotification);
    clockStatValue.setTooltip(clockDescription);
    latencyStatValue.setText(report.valid
                                 ? juce::String((int) std::round(report.totalMs)) + " ms"
                                 : juce::String("n/a"),
                             juce::dontSendNotification);
    if (report.valid) {
        // Say who chose the buffer: "Auto" means the driver's own period, so a
        // large figure is a driver fact the user can only change by picking an
        // explicit size or a different device type.
        bufferStatCaption.setText(report.requestedOutputBufferSize > 0
                                      ? juce::String("BUFFER - REQUESTED")
                                      : juce::String("BUFFER - DRIVER DEFAULT"),
                                  juce::dontSendNotification);
        bufferStatValue.setText((report.inputBufferSize > 0
                                     ? juce::String(report.inputBufferSize) + " in / "
                                     : juce::String())
                                    + juce::String(report.outputBufferSize) + " out",
                                juce::dontSendNotification);
    } else {
        bufferStatCaption.setText("BUFFER", juce::dontSendNotification);
        bufferStatValue.setText("n/a", juce::dontSendNotification);
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

    // Device errors and counted glitches are different kinds of bad news: a
    // device error means audio is not flowing, a counter means it stuttered.
    juce::String deviceIssues;
    for (const auto& [stripName, stripError] : audioEngine.getDeviceErrors())
        deviceIssues += stripName + ": " + stripError + "\n";
    deviceIssues = deviceIssues.trim();

    juce::String countedIssues;
    if (report.underruns + report.overruns > 0) countedIssues += "Glitches: " + juce::String((juce::int64)report.overruns) + " overruns / " + juce::String((juce::int64)report.underruns) + " underruns\n";
    const auto fifo = audioEngine.getFifoDiagnostics();
    if (fifo.inputUnderruns > 0 || fifo.inputOverruns > 0) {
        countedIssues += "Input capture: " + juce::String((juce::int64)fifo.inputUnderruns)
            + " underruns / " + juce::String((juce::int64)fifo.inputOverruns)
            + " overruns\n";
    }
    if (fifo.outputLowWatermarks > 0 || fifo.renderMisses > 0) {
        countedIssues += "Output safety: " + juce::String((juce::int64)fifo.outputLowWatermarks)
            + " low-watermark events / " + juce::String((juce::int64)fifo.renderMisses)
            + " render misses\n";
    }
    countedIssues = countedIssues.trim();

    juce::String issues;
    juce::Colour issuesColour = theme::green();
    if (deviceIssues.isNotEmpty()) {
        issues = deviceIssues;
        issuesColour = theme::red();
        if (countedIssues.isNotEmpty()) issues += "\n" + countedIssues;
    } else if (countedIssues.isNotEmpty()) {
        issues = countedIssues;
        issuesColour = theme::amber();
    } else {
        issues = "No audio errors.";
    }
    relayout |= setWrappedText(issuesText, issues, issuesColour);

    // The header keeps the answer visible when the body is scrolled down to the
    // tuning or device sections.
    if (audioEngine.isOpeningDevices())
        header.setStatus("Starting audio...", theme::amber());
    else if (deviceIssues.isNotEmpty())
        header.setStatus("Device error", theme::red());
    else if (countedIssues.isNotEmpty())
        header.setStatus("Audio glitches", theme::amber());
    else
        header.setStatus("No audio errors", theme::green());

    // Render load says how much of each block the engine is spending. High p99
    // is the honest reason a smaller buffer glitches, so it belongs next to
    // the latency budget rather than in a log.
    const auto render = audioEngine.getRenderTelemetry();
    if (render.count > 0) {
        renderLoadLabel.setText("Render p99: " + juce::String(render.renderP99Ms, 2) + " ms  -  "
                                    + juce::String(render.renderWorstOverBlockPercent, 0)
                                    + "% of a block  -  " + juce::String(render.renderMaxMs, 1)
                                    + " ms max",
                                juce::dontSendNotification);
        renderLoadLabel.setColour(juce::Label::textColourId,
                                  render.renderWorstOverBlockPercent > 70.0 ? theme::amber()
                                                                            : theme::dim());
    } else {
        renderLoadLabel.setText("Render load: waiting for audio", juce::dontSendNotification);
        renderLoadLabel.setColour(juce::Label::textColourId, theme::dim());
    }

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

    const auto profile = audioEngine.getLatencyProfileState();
    profileButton.setButtonText(profile.active ? "Cancel search" : "Find low-latency setting");
    profileButton.setEnabled(!busy);

    const auto asioDrivers = audioEngine.getAsioDeviceNames();
    forceAsioButton.setEnabled(!busy && !asioDrivers.isEmpty());
    if (forceAsioStatus_.isNotEmpty()) {
        relayout |= setWrappedText(asioStatusLabel, forceAsioStatus_,
                                   forceAsioStatus_.startsWith("Clock is now") ? theme::green()
                                                                              : theme::amber());
    } else if (asioDrivers.isEmpty()) {
        relayout |= setWrappedText(asioStatusLabel,
                                   "No ASIO driver installed - using Windows Audio.",
                                   theme::dim());
    } else {
        relayout |= setWrappedText(asioStatusLabel,
                                   "ASIO available: " + asioDrivers.joinIntoString(", "),
                                   theme::dim());
    }
    if (profile.active) {
        relayout |= setWrappedText(profileStatusLabel,
                                   "Step " + juce::String(profile.stepIndex) + " of "
                                       + juce::String(profile.totalSteps) + " - "
                                       + juce::String(profile.currentBuffer) + " samples - "
                                       + juce::String(juce::jmax(0, profile.remainingMs) / 1000)
                                       + " s left",
                                   theme::amber());
    } else if (profile.failed) {
        relayout |= setWrappedText(profileStatusLabel,
                                   "Glitched - back at " + juce::String(profile.currentBuffer)
                                       + " samples",
                                   theme::red());
    } else if (profile.finished) {
        relayout |= setWrappedText(profileStatusLabel,
                                   "Stable at " + juce::String(profile.currentBuffer) + " samples",
                                   theme::green());
    } else {
        relayout |= setWrappedText(profileStatusLabel,
                                   "Finds the smallest buffer this device survives.",
                                   theme::dim());
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
    if (!rackPreviewModeCombo.isPopupActive()) {
        using pluginrack::RackPreviewMode;
        const auto mode = audioEngine.getRackPreviewMode();
        rackPreviewModeCombo.setSelectedId(mode == RackPreviewMode::AllLive ? 3
                                            : mode == RackPreviewMode::Rotate ? 2
                                                                             : 1,
                                            juce::dontSendNotification);
    }
    if (!peakHoldCombo.isPopupActive())
        peakHoldCombo.setSelectedId(LevelMeter::peakHoldMs(), juce::dontSendNotification);
    reloadAllButton.setEnabled(!busy);
    reloadDevicesViewport.setEnabled(!busy);
    updatePendingState();
    refreshReloadDevices();
    refreshListenChoices();
    updateMeasurement(report);
    pollAutoReload();

    // Wrapped lines decide the section heights, so only a change in one of them
    // is worth re-running the layout.
    if (relayout) layoutBodyToFit();
}

bool SettingsView::setWrappedText(WrappedLabel& label, const juce::String& text,
                                  juce::Colour colour) {
    if (label.getText() == text && label.getTextColour() == colour) return false;
    label.setText(text, colour);
    return true;
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
    for (const auto& name : audioEngine.getAssignedInputDeviceNames())
        names.addIfNotAlreadyThere(name);
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
    budgetView.setReport(report);

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

        const bool failing =
            device.isInput
                ? audioEngine.getChannelDeviceState(device.stripId).error.isNotEmpty()
                : audioEngine.getOutputDeviceState(device.stripId).error.isNotEmpty();

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

void SettingsView::timerCallback() {
    if (audioEngine.isLatencyProfileActive()) (void) audioEngine.tickLatencyProfile();
    refresh();
}

void SettingsView::resized() {
    auto area = getLocalBounds().reduced(theme::ui::kPagePad);
    auto headerRow = area.removeFromTop(theme::ui::kHeaderH);
    // Staged bar lives in the header: pending text takes the middle, Apply
    // and Revert pin to the right, so they stay on screen while the body
    // scrolls through Buffer Size and Latency Tuning.
    applyButton.setBounds(headerRow.removeFromRight(90));
    headerRow.removeFromRight(theme::ui::kRowGap);
    revertButton.setBounds(headerRow.removeFromRight(90));
    headerRow.removeFromRight(theme::ui::kSectionGap);
    pendingLabel.setBounds(headerRow.removeFromRight(juce::jmax(0, headerRow.getWidth() / 2)));
    header.setBounds(headerRow);
    area.removeFromTop(theme::ui::kHeaderGap);
    viewport.setBounds(area);
    layoutBodyToFit();
}

void SettingsView::layoutBodyToFit() {
    const int available = viewport.getWidth();
    if (available <= 0) return;

    const int height = layoutBody(available);
    // A scrollbar steals width, which can push a wrapped line onto another
    // line. Lay out once more at the narrower width so the two agree.
    if (height > viewport.getHeight())
        layoutBody(juce::jmax(1, available - viewport.getScrollBarThickness()));
}

int SettingsView::layoutBody(int width) {
    constexpr int pad = theme::ui::kPanelPad;
    constexpr int kStatH = 34;
    constexpr int kToggleH = 24;
    constexpr int kToggleGap = 4;
    constexpr int kDeviceRowH = 24;
    constexpr int kMaxDeviceRows = 6;
    const int innerW = juce::jmax(1, width - 2 * pad);

    body.setSize(width, body.getHeight());

    std::vector<juce::Rectangle<int>> panels;
    int y = 0;

    // A section is its caption and then the panel under it: the caption is a
    // label so it scrolls with the content it names.
    const auto startSection = [&](juce::Label& caption, juce::Label* badge) {
        caption.setBounds(0, y, width, theme::ui::kSectionH);
        if (badge != nullptr) {
            // The BETA tag follows the caption's own width so the two never
            // overlap as the words change.
            const int textWidth =
                juce::GlyphArrangement::getStringWidthInt(caption.getFont(), caption.getText());
            badge->setBounds(textWidth + 8, y, 44, theme::ui::kSectionH);
        }
        y += theme::ui::kSectionH + theme::ui::kRowGap;
        return y;
    };
    const auto endSection = [&](juce::Rectangle<int> panel) {
        panels.push_back(panel);
        y = panel.getBottom() + theme::ui::kSectionGap;
    };

    // AUDIO STATUS
    {
        const int panelTop = startSection(statusCaption, nullptr);
        const int issuesH =
            WrappedLabel::heightFor(issuesText.getText(), innerW, issuesText.getFont());
        const int panelH = pad + kStatH + (issuesH > 0 ? 10 + issuesH : 0) + pad;
        const auto panel = juce::Rectangle<int>(0, panelTop, width, panelH);

        auto inner = panel.reduced(pad);
        auto stats = inner.removeFromTop(kStatH);
        constexpr int kStatGap = 18;
        const auto placeStat = [](juce::Rectangle<int> cell, juce::Label& caption,
                                  juce::Label& value) {
            caption.setBounds(cell.removeFromTop(13));
            value.setBounds(cell.removeFromTop(18));
        };
        // The clock is the lead: half the row, then the two numbers that say
        // how it is performing, so the three never drift apart.
        auto clockCell = stats.removeFromLeft(stats.getWidth() / 2);
        stats.removeFromLeft(kStatGap);
        const int statW = (stats.getWidth() - kStatGap) / 2;
        auto latencyCell = stats.removeFromLeft(statW);
        stats.removeFromLeft(kStatGap);
        placeStat(clockCell, clockStatCaption, clockStatValue);
        placeStat(latencyCell, latencyStatCaption, latencyStatValue);
        placeStat(stats, bufferStatCaption, bufferStatValue);

        if (issuesH > 0)
            issuesText.setBounds(inner.removeFromTop(10 + issuesH).withTrimmedTop(10));
        else
            issuesText.setBounds({});
        endSection(panel);
    }

    // BUFFER SIZE: only the two buffer combos live here now. The staged
    // Apply/Revert bar moved into the page header, so the row never crushes
    // the "Unapplied changes" label and the Latency Tuning toggles below
    // share the same Apply step.
    {
        const int panelTop = startSection(bufferCaption, nullptr);
        const int panelH = pad + theme::ui::kControlH + pad;
        const auto panel = juce::Rectangle<int>(0, panelTop, width, panelH);
        auto inner = panel.reduced(pad);

        auto row = inner.removeFromTop(theme::ui::kControlH);
        inputBufferSizeLabel.setBounds(row.removeFromLeft(theme::ui::kLabelW));
        inputBufferSizeCombo.setBounds(row.removeFromLeft(150));
        row.removeFromLeft(18);
        outputBufferSizeLabel.setBounds(row.removeFromLeft(theme::ui::kLabelW));
        outputBufferSizeCombo.setBounds(row.removeFromLeft(150));
        endSection(panel);
    }

    // ROUND-TRIP LATENCY
    {
        const int panelTop = startSection(roundTripLabel, &roundTripBeta);
        constexpr int kHeadlineH = 30;
        constexpr int kDetailH = 16;
        const int budgetH = hasBudget_ ? LatencyBudgetView::preferredHeight() : 0;
        const int panelH = pad + kHeadlineH + 2 + kDetailH
                           + (budgetH > 0 ? 8 + budgetH + 6 : 10) + kDetailH + pad;
        const auto panel = juce::Rectangle<int>(0, panelTop, width, panelH);
        auto inner = panel.reduced(pad);

        auto headline = inner.removeFromTop(kHeadlineH);
        measureButton.setBounds(headline.removeFromRight(theme::ui::kMeasureW)
                                    .withSizeKeepingCentre(theme::ui::kMeasureW,
                                                           theme::ui::kControlH));
        headline.removeFromRight(10);
        listenCombo.setBounds(headline.removeFromRight(200)
                                  .withSizeKeepingCentre(200, theme::ui::kControlH));
        headline.removeFromRight(theme::ui::kRowGap);
        listenLabel.setBounds(headline.removeFromRight(64)
                                  .withSizeKeepingCentre(64, theme::ui::kControlH));
        headline.removeFromRight(12);
        measuredLabel.setBounds(headline.withSizeKeepingCentre(headline.getWidth(), 26));

        inner.removeFromTop(2);
        measuredDetailLabel.setBounds(inner.removeFromTop(kDetailH));
        budgetView.setVisible(budgetH > 0);
        if (budgetH > 0) {
            inner.removeFromTop(8);
            // The stage list is read label to value; on a wide window that
            // distance would be a screen wide, so the rows keep a readable
            // measure instead of stretching to the panel edge.
            budgetView.setBounds(inner.removeFromTop(budgetH).removeFromLeft(
                juce::jmin(inner.getWidth(), 760)));
            inner.removeFromTop(6);
        } else {
            budgetView.setBounds({});
            inner.removeFromTop(10);
        }
        renderLoadLabel.setBounds(inner.removeFromTop(kDetailH));
        endSection(panel);
    }

    // LATENCY TUNING (OPT-IN)
    {
        const int panelTop = startSection(tuningLabel, &tuningBeta);
        constexpr int kActionW = 200;
        const int statusW = juce::jmax(120, innerW - kActionW - theme::ui::kSectionGap);
        const int profileStatusH = WrappedLabel::heightFor(profileStatusLabel.getText(), statusW,
                                                           profileStatusLabel.getFont());
        const int asioStatusH = WrappedLabel::heightFor(asioStatusLabel.getText(), statusW,
                                                        asioStatusLabel.getFont());
        const int profileRowH = juce::jmax(theme::ui::kControlH, profileStatusH);
        const int asioRowH = juce::jmax(theme::ui::kControlH, asioStatusH);
        constexpr int kToggleW = 430;
        constexpr int kToggleColGap = 24;
        // Five opt-in switches read as one list in a narrow window and as rows
        // of two when the panel is wide enough to hold them side by side.
        const bool twoColumns = innerW >= 2 * kToggleW + kToggleColGap;
        const int toggleRows = twoColumns ? 3 : 5;
        const int togglesH = toggleRows * kToggleH + (toggleRows - 1) * kToggleGap;
        const int panelH = pad + togglesH + 10 + profileRowH + 6 + asioRowH + pad;
        const auto panel = juce::Rectangle<int>(0, panelTop, width, panelH);
        auto inner = panel.reduced(pad);

        const std::array<juce::ToggleButton*, 5> toggles{{
            &lowLatencyInputToggle, &exclusiveToggle, &lowLatencyOutputsToggle,
            &minimalClockFifoToggle, &fullDuplexToggle,
        }};
        const int columnW = twoColumns ? (innerW - kToggleColGap) / 2 : innerW;
        for (int i = 0; i < (int) toggles.size(); ++i) {
            const int column = twoColumns ? i % 2 : 0;
            const int line = twoColumns ? i / 2 : i;
            toggles[i]->setBounds(column * (columnW + kToggleColGap),
                                  inner.getY() + line * (kToggleH + kToggleGap),
                                  juce::jmin(kToggleW, columnW), kToggleH);
        }
        inner.removeFromTop(togglesH);

        inner.removeFromTop(10);
        auto profileRow = inner.removeFromTop(profileRowH);
        profileButton.setBounds(profileRow.removeFromLeft(kActionW)
                                    .withSizeKeepingCentre(kActionW, theme::ui::kControlH));
        profileRow.removeFromLeft(theme::ui::kSectionGap);
        profileStatusLabel.setBounds(profileRow.withSizeKeepingCentre(
            profileRow.getWidth(), juce::jmax(1, profileStatusH)));

        inner.removeFromTop(6);
        auto asioRow = inner.removeFromTop(asioRowH);
        forceAsioButton.setBounds(asioRow.removeFromLeft(kActionW)
                                      .withSizeKeepingCentre(kActionW, theme::ui::kControlH));
        asioRow.removeFromLeft(theme::ui::kSectionGap);
        asioStatusLabel.setBounds(
            asioRow.withSizeKeepingCentre(asioRow.getWidth(), juce::jmax(1, asioStatusH)));
        endSection(panel);
    }

    // APP
    {
        const int panelTop = startSection(appCaption, nullptr);
        const int panelH = pad + theme::ui::kControlH + 6 + theme::ui::kControlH + pad;
        const auto panel = juce::Rectangle<int>(0, panelTop, width, panelH);
        auto inner = panel.reduced(pad);

        auto row = inner.removeFromTop(theme::ui::kControlH);
        rackThumbnailsLabel.setBounds(row.removeFromLeft(theme::ui::kLabelW));
        rackThumbnailFpsCombo.setBounds(row.removeFromLeft(90));
        row.removeFromLeft(24);
        rackPreviewModeLabel.setBounds(row.removeFromLeft(theme::ui::kLabelW));
        rackPreviewModeCombo.setBounds(row.removeFromLeft(140));

        inner.removeFromTop(6);
        auto row2 = inner.removeFromTop(theme::ui::kControlH);
        peakHoldLabel.setBounds(row2.removeFromLeft(theme::ui::kLabelW));
        peakHoldCombo.setBounds(row2.removeFromLeft(90));
        row2.removeFromLeft(24);
        closeToTrayToggle.setBounds(row2.removeFromLeft(320));
        endSection(panel);
    }

    // AUDIO DEVICES
    {
        const int panelTop = startSection(devicesLabel, nullptr);
        const bool hasDevices = !reloadDevices_.empty();
        const int rows = juce::jlimit(0, kMaxDeviceRows, (int) reloadDevices_.size());
        const int listH = rows * kDeviceRowH;
        const int panelH = pad + theme::ui::kControlH + (hasDevices ? 6 + listH : 0) + 6
                           + theme::ui::kControlH + pad;
        const auto panel = juce::Rectangle<int>(0, panelTop, width, panelH);
        auto inner = panel.reduced(pad);

        reloadAllButton.setBounds(inner.removeFromTop(theme::ui::kControlH).removeFromLeft(220));

        // The list sizes to its rows; only a long list needs its own scrollbar,
        // which keeps the page's scroll the one the wheel reaches for.
        reloadDevicesViewport.setVisible(hasDevices);
        if (hasDevices) {
            inner.removeFromTop(6);
            reloadDevicesViewport.setBounds(inner.removeFromTop(listH).removeFromLeft(430));
        } else {
            reloadDevicesViewport.setBounds({});
        }

        inner.removeFromTop(6);
        autoReloadToggle.setBounds(inner.removeFromTop(theme::ui::kControlH).removeFromLeft(380));
        endSection(panel);
    }

    body.setPanels(std::move(panels));
    body.setSize(width, y);
    return y;
}
