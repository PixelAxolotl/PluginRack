#include "MainComponent.h"

#include <vector>

#include "SceneBridge.h"

ToolbarComponent::ToolbarComponent() {
    for (auto* b : { &racksButton, &mixerButton, &routingButton, &settingsButton })
        b->setClickingTogglesState(true);

    racksButton.onClick = [this] { selectView(AppView::Rack); };
    mixerButton.onClick = [this] { selectView(AppView::Mixer); };
    routingButton.onClick = [this] { selectView(AppView::Routing); };
    settingsButton.onClick = [this] { selectView(AppView::Settings); };

    racksButton.setTooltip("Rack view: plugin chains");
    mixerButton.setTooltip("Mixer view: faders, meters, inline routing");
    routingButton.setTooltip("Routing view: full channel/output matrix");
    settingsButton.setTooltip("Settings view: device, buffer, diagnostics");

    snapshotButton.setTooltip("Snapshots arrive in a later task");
    snapshotButton.onClick = [this] { if (onSnapshot) onSnapshot(); };

    latencyLabel.setJustificationType(juce::Justification::centredRight);
    latencyLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    latencyLabel.setColour(juce::Label::textColourId, theme::dim());

    errorButton.setColour(juce::TextButton::textColourOffId, theme::red());
    errorButton.setTooltip("Audio errors — click for details");
    errorButton.setVisible(false);
    errorButton.onClick = [this] { if (onErrorClicked) onErrorClicked(); };

    saveButton.onClick = [this] { if (onSave) onSave(); };
    loadButton.onClick = [this] { if (onLoad) onLoad(); };
    menuButton.onClick = [this] { if (onMenu) onMenu(); };

    addAndMakeVisible(racksButton);
    addAndMakeVisible(mixerButton);
    addAndMakeVisible(routingButton);
    addAndMakeVisible(settingsButton);
    addAndMakeVisible(snapshotButton);
    addAndMakeVisible(latencyLabel);
    addAndMakeVisible(errorButton);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(loadButton);
    addAndMakeVisible(menuButton);

    updateNavToggles();
}

void ToolbarComponent::selectView(AppView view) {
    setView(view);
    if (onViewSelected) onViewSelected(view);
}

void ToolbarComponent::setView(AppView view) {
    currentView = view;
    updateNavToggles();
}

void ToolbarComponent::updateNavToggles() {
    racksButton.setToggleState(currentView == AppView::Rack, juce::dontSendNotification);
    mixerButton.setToggleState(currentView == AppView::Mixer, juce::dontSendNotification);
    routingButton.setToggleState(currentView == AppView::Routing, juce::dontSendNotification);
    settingsButton.setToggleState(currentView == AppView::Settings, juce::dontSendNotification);
}

void ToolbarComponent::setClockText(const juce::String& text, bool isWarning) {
    clockText = text;
    clockColour = isWarning ? theme::amber() : theme::dim();
    repaint();
}

void ToolbarComponent::setLatencyText(const juce::String& text, juce::Colour colour, const juce::String& tooltip) {
    latencyLabel.setText(text, juce::dontSendNotification);
    latencyLabel.setColour(juce::Label::textColourId, colour);
    latencyLabel.setTooltip(tooltip);
}

void ToolbarComponent::setHasError(bool hasError) {
    if (errorButton.isVisible() == hasError) return;
    errorButton.setVisible(hasError);
    resized();
}

void ToolbarComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::panel());
    g.setColour(theme::border());
    g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());

    // wordmark
    g.setColour(theme::green());
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("PluginRack", 20, 0, 182, getHeight(), juce::Justification::centredLeft);

    // clock info (single line, clipped — same as the old header paint)
    g.setColour(clockColour);
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawFittedText(clockText, clockBounds, juce::Justification::centredRight, 1);
}

void ToolbarComponent::resized() {
    auto area = getLocalBounds().reduced(8, 12);
    area.removeFromLeft(190); // painted wordmark space

    racksButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    mixerButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    routingButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    settingsButton.setBounds(area.removeFromLeft(64));

    menuButton.setBounds(area.removeFromRight(60));
    area.removeFromRight(8);
    loadButton.setBounds(area.removeFromRight(50));
    area.removeFromRight(4);
    saveButton.setBounds(area.removeFromRight(50));
    area.removeFromRight(8);
    snapshotButton.setBounds(area.removeFromRight(76));
    area.removeFromRight(8);
    if (errorButton.isVisible()) {
        errorButton.setBounds(area.removeFromRight(28));
        area.removeFromRight(8);
    }
    latencyLabel.setBounds(area.removeFromRight(128));
    clockBounds = area;
}

ViewRouter::PlaceholderView::PlaceholderView(const juce::String& title, const juce::String& subtitle) {
    titleLabel.setText(title, juce::dontSendNotification);
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(titleLabel);

    subtitleLabel.setJustificationType(juce::Justification::centred);
    subtitleLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    subtitleLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(subtitleLabel);

    setSubtitle(subtitle);
}

void ViewRouter::PlaceholderView::setSubtitle(const juce::String& subtitle) {
    subtitleLabel.setText(subtitle, juce::dontSendNotification);
}

void ViewRouter::PlaceholderView::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
}

void ViewRouter::PlaceholderView::resized() {
    auto area = getLocalBounds().reduced(24);
    titleLabel.setBounds(area.removeFromTop(30));
    area.removeFromTop(4);
    subtitleLabel.setBounds(area.removeFromTop(22));
}

ViewRouter::ViewRouter(AudioEngine& engine, juce::Component& mixerViewIn)
    : audioEngine(engine)
    , mixerView(mixerViewIn)
    , rackView(audioEngine)
    , routingView("Routing", "The full channel/output matrix arrives in Task 5. Per-channel routing stays in the mixer strips.")
    , settingsView("Settings", "Device, buffer and diagnostics arrive in Task 5. Audio details stay in the toolbar menu.") {
    addAndMakeVisible(mixerView);
    addAndMakeVisible(rackView);
    addAndMakeVisible(routingView);
    addAndMakeVisible(settingsView);
    refreshViews();
    setView(AppView::Rack);
}

void ViewRouter::setView(AppView view) {
    currentView = view;
    mixerView.setVisible(view == AppView::Mixer);
    rackView.setVisible(view == AppView::Rack);
    routingView.setVisible(view == AppView::Routing);
    settingsView.setVisible(view == AppView::Settings);
}

void ViewRouter::refreshViews() {
    rackView.rebuild();
}

void ViewRouter::resized() {
    mixerView.setBounds(getLocalBounds());
    rackView.setBounds(getLocalBounds());
    routingView.setBounds(getLocalBounds());
    settingsView.setBounds(getLocalBounds());
}

MainComponent::MainComponent()
    : mixer(audioEngine), router(audioEngine, mixer) {
    addAndMakeVisible(toolbar);
    addAndMakeVisible(router);

    toolbar.setView(AppView::Rack);
    toolbar.onViewSelected = [this](AppView view) { router.setView(view); };
    toolbar.onSave = [this] { saveScene(); };
    toolbar.onLoad = [this] { loadScene(true); };
    toolbar.onMenu = [this] { showMenu(); };
    toolbar.onSnapshot = [this] { showSnapshotPlaceholder(); };
    toolbar.onErrorClicked = [this] { showAudioErrors(); };

    mixer.onStructureChanged = [this] { refreshViews(); };

    loadScene(false);
    startTimerHz(4);
    timerCallback(); // populate the toolbar readouts before the first tick
    setSize(1600, 900);
}

void MainComponent::timerCallback() {
    const auto clockDesc = audioEngine.getClockDescription();
    toolbar.setClockText(clockDesc, clockDesc.startsWith("No output"));

    const auto report = audioEngine.getLatencyReport();
    if (!report.valid) {
        smoothedLatencyMs = -1.0;
        toolbar.setLatencyText({}, theme::dim(), {});
    } else {
        // A live glitch counter overrides smoothing: snap to the raw total so
        // the EMA never hides an active underrun/overrun.
        const bool glitch = (report.underruns + report.overruns) > 0;
        if (glitch || smoothedLatencyMs < 0.0) smoothedLatencyMs = report.totalMs;
        else smoothedLatencyMs += 0.25 * (report.totalMs - smoothedLatencyMs);
        juce::String latText = "LAT ~" + juce::String((int)std::round(smoothedLatencyMs)) + " ms (est.)";
        if (glitch) latText += " !";
        juce::Colour colour = theme::dim();
        if (glitch) colour = theme::red();
        else if (smoothedLatencyMs > 60.0) colour = theme::red();
        else if (smoothedLatencyMs > 30.0) colour = theme::amber();
        toolbar.setLatencyText(latText, colour,
                                "Estimated (approx.) input to output: input " +
                                juce::String((int)std::round(report.inputDeviceMs)) + " ms (buffer " +
                                juce::String(report.inputBufferSize) + " smp) + plugins " +
                                juce::String((int)std::round(report.pluginMs)) + " ms + output " +
                                juce::String((int)std::round(report.outputDeviceMs)) + " ms (buffer " +
                                juce::String(report.outputBufferSize) + " smp) + FIFO " +
                                juce::String((int)std::round(report.fifoMs)) + " ms" +
                                (glitch ? " - dropouts detected" : ""));
    }

    bool hasError = !report.valid;
    if (!hasError && (report.underruns + report.overruns) > 0) hasError = true;
    if (!hasError) {
        for (const auto& c : audioEngine.getChannels()) {
            if (c->inputError.isNotEmpty()) { hasError = true; break; }
        }
    }
    if (!hasError) {
        for (const auto& o : audioEngine.getOutputs()) {
            if (o->outputError.isNotEmpty()) { hasError = true; break; }
        }
    }
    toolbar.setHasError(hasError);
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
}

void MainComponent::resized() {
    auto area = getLocalBounds();
    toolbar.setBounds(area.removeFromTop(56));
    router.setBounds(area);
}

void MainComponent::showMenu() {
    juce::PopupMenu menu;
    menu.addItem("Save Scene", [this] { saveScene(); });
    menu.addItem("Load Scene", [this] { loadScene(true); });
    menu.addItem("Open Scenes Folder", [this] {
        const auto dir = SceneBridge::getSceneFile().getParentDirectory();
        dir.createDirectory();
        dir.startAsProcess();
    });
    menu.addSeparator();
    menu.addItem("Reset to Defaults", [this] {
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
                                            "PluginRack",
                                            "Reset engine to defaults (1 channel, 3 outputs)?",
                                            "Reset", "Cancel", nullptr,
                                            juce::ModalCallbackFunction::create([this](int result) {
                                                if (result != 1) return;
                                                std::vector<std::uint32_t> chIds;
                                                for (const auto& c : audioEngine.getChannels()) chIds.push_back(c->id);
                                                for (std::size_t i = 1; i < chIds.size(); ++i)
                                                    audioEngine.removeChannel(chIds[i]);
                                                std::vector<std::uint32_t> outIds;
                                                for (const auto& o : audioEngine.getOutputs()) outIds.push_back(o->id);
                                                for (std::size_t i = 3; i < outIds.size(); ++i)
                                                    audioEngine.removeOutput(outIds[i]);
                                                while (audioEngine.getChannels().size() < 1) audioEngine.addChannel();
                                                while (audioEngine.getOutputs().size() < 3) audioEngine.addOutput();
                                                refreshViews();
                                                saveScene();
                                            }));
    });
    menu.addSeparator();
    menu.addSectionHeader("Audio");
    menu.addItem(audioEngine.getClockDescription(), false, false, [] {});
    menu.addSectionHeader("Latency (est.)");
    const auto report = audioEngine.getLatencyReport();
    if (!report.valid) {
        menu.addItem("No output device", false, false, [] {});
    } else {
        const auto ms = [](double v) { return juce::String((int)std::round(v)) + " ms"; };
        menu.addItem("Total ~" + ms(report.totalMs), false, false, [] {});
        menu.addItem("Input device " + ms(report.inputDeviceMs) + " (buffer "
                         + juce::String(report.inputBufferSize) + " smp)",
                     false, false, [] {});
        juce::String pluginText = "Plugins " + ms(report.pluginMs);
        if (report.maxInsertId != 0) {
            pluginText += " (max " + report.maxInsertName + " " + ms(report.maxInsertMs) + ")";
        }
        menu.addItem(pluginText, false, false, [] {});
        menu.addItem("Output device " + ms(report.outputDeviceMs) + " (buffer "
                         + juce::String(report.outputBufferSize) + " smp)",
                     false, false, [] {});
        menu.addItem("FIFO " + ms(report.fifoMs), false, false, [] {});
        if (report.underruns > 0 || report.overruns > 0) {
            menu.addItem("Glitches: " + juce::String((juce::int64)report.overruns)
                             + " overruns / " + juce::String((juce::int64)report.underruns)
                             + " underruns",
                         false, false, [] {});
        }
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(toolbar.getMenuTarget()));
}

void MainComponent::showAudioErrors() {
    const auto report = audioEngine.getLatencyReport();
    juce::StringArray lines;
    if (!report.valid) {
        lines.add("No output device open.");
    } else if ((report.underruns + report.overruns) > 0) {
        lines.add("Glitches: " + juce::String((juce::int64)report.overruns) + " overruns / "
                  + juce::String((juce::int64)report.underruns) + " underruns.");
    }
    for (const auto& c : audioEngine.getChannels()) {
        if (c->inputError.isNotEmpty()) lines.add(c->name + ": " + c->inputError);
    }
    for (const auto& o : audioEngine.getOutputs()) {
        if (o->outputError.isNotEmpty()) lines.add(o->name + ": " + o->outputError);
    }
    if (lines.isEmpty()) lines.add("No audio errors.");
    showError(lines.joinIntoString("\n"));
}

void MainComponent::showSnapshotPlaceholder() {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "PluginRack",
                                           "Snapshots are not implemented yet — scene save/load stays in the toolbar.");
}

void MainComponent::saveScene() {
    const auto error = SceneBridge::save(audioEngine.makeScene());
    if (error.isNotEmpty()) showError(error);
}

void MainComponent::loadScene(bool showMissingMessage) {
    juce::String error;
    const auto saved = SceneBridge::load(error);
    if (error.isNotEmpty()) { showError(error); return; }
    if (!saved.has_value()) {
        if (showMissingMessage) showError("No scene has been saved yet.");
        return;
    }
    const auto pluginErrors = audioEngine.restoreScene(*saved);
    refreshViews();
    if (pluginErrors.isNotEmpty()) showError("The scene was loaded, but some plugins could not be restored:\n" + pluginErrors);
}

void MainComponent::refreshViews() {
    mixer.rebuild();
    router.refreshViews();
}

void MainComponent::showError(const juce::String& message) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", message);
}
