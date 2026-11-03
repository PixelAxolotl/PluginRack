#include "MainComponent.h"

#include <vector>

#include "SceneBridge.h"

ToolbarComponent::ToolbarComponent() {
    for (auto* b : { &racksButton, &mixerButton, &routingButton, &ioButton,
                     &savesButton, &settingsButton }) {
        b->setClickingTogglesState(true);
        theme::ui::styleButton(*b);
    }

    racksButton.onClick = [this] { selectView(AppView::Rack); };
    mixerButton.onClick = [this] { selectView(AppView::Mixer); };
    routingButton.onClick = [this] { selectView(AppView::Routing); };
    ioButton.onClick = [this] { selectView(AppView::IO); };
    savesButton.onClick = [this] { selectView(AppView::Saves); };
    settingsButton.onClick = [this] { selectView(AppView::Settings); };

    racksButton.setTooltip("Rack view: plugin chains");
    mixerButton.setTooltip("Mixer view: faders, meters, inline routing");
    routingButton.setTooltip("Routing view: full channel/output matrix");
    ioButton.setTooltip("I/O view: input and output devices, round-trip measurement");
    savesButton.setTooltip("Saves view: saved scenes");
    settingsButton.setTooltip("Settings view: buffer, latency tuning, diagnostics");

    snapshotButton.setTooltip("Snapshots arrive in a later task");
    snapshotButton.onClick = [this] { if (onSnapshot) onSnapshot(); };

    latencyLabel.setJustificationType(juce::Justification::centredRight);
    latencyLabel.setFont(theme::ui::captionFont());
    latencyLabel.setColour(juce::Label::textColourId, theme::dim());

    errorButton.setTooltip("Audio errors - click for details");
    errorButton.setVisible(false);
    errorButton.onClick = [this] { if (onErrorClicked) onErrorClicked(); };

    for (auto* b : { &saveButton, &snapshotButton, &menuButton })
        theme::ui::styleButton(*b);
    theme::ui::styleButton(errorButton);
    errorButton.setColour(juce::TextButton::textColourOffId, theme::red());

    saveButton.onClick = [this] { if (onSave) onSave(); };
    menuButton.onClick = [this] { if (onMenu) onMenu(); };

    addAndMakeVisible(racksButton);
    addAndMakeVisible(mixerButton);
    addAndMakeVisible(routingButton);
    addAndMakeVisible(ioButton);
    addAndMakeVisible(settingsButton);
    addAndMakeVisible(snapshotButton);
    addAndMakeVisible(latencyLabel);
    addAndMakeVisible(errorButton);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(savesButton);
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
    ioButton.setToggleState(currentView == AppView::IO, juce::dontSendNotification);
    savesButton.setToggleState(currentView == AppView::Saves, juce::dontSendNotification);
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

    // clock info (single line, clipped - same as the old header paint)
    g.setColour(clockColour);
    g.setFont(theme::ui::captionFont());
    g.drawFittedText(clockText, clockBounds, juce::Justification::centredRight, 1);
}

void ToolbarComponent::resized() {
    auto area = getLocalBounds().reduced(8, 12);
    area.removeFromLeft(190); // painted wordmark space

    racksButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    mixerButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    ioButton.setBounds(area.removeFromLeft(48));
    area.removeFromLeft(4);
    routingButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    savesButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    settingsButton.setBounds(area.removeFromLeft(64));

    menuButton.setBounds(area.removeFromRight(60));
    area.removeFromRight(8);
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

ViewRouter::ViewRouter(AudioEngine& engine, MixerComponent& mixerViewIn)
    : audioEngine(engine)
    , mixerView(mixerViewIn)
    , rackView(audioEngine)
    , routingView(audioEngine)
    , ioView(audioEngine)
    , savesView([this](const pluginrack::Scene& scene) {
          return onRestoreRequested ? onRestoreRequested(scene) : juce::String();
      })
    , settingsView(audioEngine) {
    rackView.onJumpToMixer = [this] {
        setView(AppView::Mixer);
        if (onViewChanged) onViewChanged(AppView::Mixer);
    };
    rackView.onStructureChanged = [this] {
        if (onStructureChanged) onStructureChanged();
    };
    // Output/Sub strips erase inserts the rack may have docked, and they
    // cannot reach the rack directly. The InsertHost remove button fires
    // this release-only hook synchronously before the engine frees the
    // processor; a full refreshViews here would re-dock the doomed insert
    // before the erase, so this releases without rebuilding.
    pluginRemovingHook() = [this] { releaseDockedEditor(); };
    ioView.onStructureChanged = [this] {
        // Strips added, removed or renamed on the I/O page change every view.
        if (onStructureChanged) onStructureChanged();
    };
    mixerView.onSelectionChanged = [this](int id, bool isOutput) {
        rackView.setSelectedChannel(id, isOutput);
    };
    mixerView.onDoubleClicked = [this](int id, bool isOutput) {
        // Channels and outputs both open on their rack page.
        rackView.setSelectedChannel(id, isOutput);
        setView(AppView::Rack);
    };
    addAndMakeVisible(mixerView);
    addAndMakeVisible(rackView);
    addAndMakeVisible(routingView);
    addAndMakeVisible(ioView);
    addAndMakeVisible(savesView);
    addAndMakeVisible(settingsView);
    refreshViews();
    setView(AppView::Rack);
}

void ViewRouter::setView(AppView view) {
    currentView = view;
    if (view == AppView::Rack) rackView.rebuild();
    if (view == AppView::Mixer) mixerView.rebuild();
    if (view == AppView::Routing) routingView.rebuild();
    if (view == AppView::IO) ioView.rebuild();
    if (view == AppView::Saves) savesView.rebuild();
    if (view == AppView::Settings) settingsView.refresh();
    mixerView.setVisible(view == AppView::Mixer);
    rackView.setVisible(view == AppView::Rack);
    routingView.setVisible(view == AppView::Routing);
    ioView.setVisible(view == AppView::IO);
    savesView.setVisible(view == AppView::Saves);
    settingsView.setVisible(view == AppView::Settings);
}

void ViewRouter::refreshViews() {
    // Belt and braces alongside the pre-erase hook: never rebuild the rack
    // while it still holds an editor for an insert the engine has freed
    // (scene restore already releases before its strip swap for the same
    // reason). Null-guarded, so a second release is a no-op; releasing only
    // deletes the editor and never calls back into the views.
    releaseDockedEditor();
    rackView.rebuild();
    routingView.rebuild();
    ioView.rebuild();
    savesView.rebuild();
    settingsView.refresh();
}

void ViewRouter::refreshDeviceStrips() {
    mixerView.refreshStrips();
    ioView.refreshDeviceStrips();
}

void ViewRouter::resized() {
    mixerView.setBounds(getLocalBounds());
    rackView.setBounds(getLocalBounds());
    routingView.setBounds(getLocalBounds());
    ioView.setBounds(getLocalBounds());
    savesView.setBounds(getLocalBounds());
    settingsView.setBounds(getLocalBounds());
}

MainComponent::MainComponent()
    : mixer(audioEngine), router(audioEngine, mixer) {
    addAndMakeVisible(toolbar);
    addAndMakeVisible(router);

    toolbar.setView(AppView::Rack);
    toolbar.onViewSelected = [this](AppView view) { router.setView(view); };
    router.onViewChanged = [this](AppView view) { toolbar.setView(view); };
    router.onStructureChanged = [this] { refreshViews(); };
    router.onRestoreRequested = [this](const pluginrack::Scene& scene) { return applyScene(scene); };
    toolbar.onSave = [this] {
        auto names = SceneBridge::getSaveNames();
        int n = 1;
        while (names.contains("Save " + juce::String(n))) ++n;
        const auto error = SceneBridge::saveNamed(audioEngine.makeScene(), "Save " + juce::String(n));
        if (error.isNotEmpty()) showError(error);
        selectView(AppView::Saves);
        router.refreshViews();
    };
    toolbar.onMenu = [this] { showMenu(); };
    toolbar.onSnapshot = [this] { showSnapshotPlaceholder(); };
    toolbar.onErrorClicked = [this] { showAudioErrors(); };

    mixer.onStructureChanged = [this] { refreshViews(); };

    // All device/plugin work is deferred to the first paint (see paint()):
    // starting it from the constructor ran it before the window had drawn,
    // which is what made startup look like a hang.
    startTimerHz(4);
    timerCallback(); // populate the toolbar readouts before the first tick
    setSize(1600, 900);
}

void MainComponent::timerCallback() {
    audioEngine.pollLatencyMeasurement();
    if (audioEngine.isOpeningDevices()) {
        const auto total = audioEngine.getDeviceOpenTotal();
        const auto done = audioEngine.getDeviceOpenProgress();
        const auto status = audioEngine.getDeviceOpenStatus();
        juce::String text = "Starting audio... " + juce::String(done) + "/"
                          + juce::String(total);
        if (status.isNotEmpty()) text += "  " + status;
        toolbar.setClockText(text, true);
    } else {
        if (audioEngine.consumeDeviceOpenFinished()) {
            const auto deviceErrors = audioEngine.takeDeviceOpenErrors();
            // The strips were built before the background open finished, so
            // their device readouts still show the pre-open state. Refresh them
            // now: a device that no longer exists has to turn red, not stay
            // green.
            router.refreshDeviceStrips();
            if (deviceErrors.isNotEmpty())
                showError("Some audio devices could not be opened:\n" + deviceErrors);
        }
        const auto clockDesc = audioEngine.getClockDescription();
        toolbar.setClockText(clockDesc, clockDesc.startsWith("No output"));
    }

    // A device can vanish while its stream is open (unplugged, disabled in
    // Windows) and the strips would keep showing it as healthy. Check a few
    // times a minute and flip the affected strips to red.
    if (!audioEngine.isOpeningDevices() && ++availabilityTicks_ >= 40) {
        availabilityTicks_ = 0;
        if (audioEngine.refreshDeviceAvailability()) router.refreshDeviceStrips();
    }

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
        const juce::String estimateTip = "Estimated (approx.) input to output: input " +
                            juce::String((int)std::round(report.inputDeviceMs)) + " ms (buffer " +
                            juce::String(report.inputBufferSize) + " smp) + plugins " +
                            juce::String((int)std::round(report.pluginMs)) + " ms + output " +
                            juce::String((int)std::round(report.outputDeviceMs)) + " ms (buffer " +
                            juce::String(report.outputBufferSize) + " smp) + output FIFO " +
                            juce::String((int)std::round(report.fifoMs)) + " ms" +
                            (glitch ? " - dropouts detected" : "");
        juce::String latText;
        juce::String tip;
        if (report.measuredValid) {
            latText = "LAT " + juce::String((int)std::round(report.measuredMs)) + " ms";
            tip = "Measured round-trip: " + juce::String((int)std::round(report.measuredMs))
                + " ms (" + estimateTip + ")";
        } else {
            latText = "LAT ~" + juce::String((int)std::round(smoothedLatencyMs)) + " ms (est.)";
            tip = estimateTip;
        }
        if (glitch) latText += " !";
        juce::Colour colour = theme::dim();
        if (glitch) colour = theme::red();
        else if (smoothedLatencyMs > 60.0) colour = theme::red();
        else if (smoothedLatencyMs > 30.0) colour = theme::amber();
        toolbar.setLatencyText(latText, colour, tip);
    }

    bool hasError = !report.valid;
    if (!hasError && (report.underruns + report.overruns) > 0) hasError = true;
    if (!hasError) hasError = !audioEngine.getDeviceErrors().empty();
    toolbar.setHasError(hasError);
}

void MainComponent::selectView(AppView view) {
    toolbar.setView(view);
    router.setView(view);
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
    // Queue the audio startup only once the first frame has been drawn: a
    // restore running from the constructor paints nothing until it finishes.
    if (!audioInitQueued_) {
        audioInitQueued_ = true;
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr) safe->initialiseAudio();
        });
    }
}

void MainComponent::resized() {
    auto area = getLocalBounds();
    toolbar.setBounds(area.removeFromTop(56));
    router.setBounds(area);
}

void MainComponent::showMenu() {
    juce::PopupMenu menu;
    menu.addItem("Save Scene As...", [this] { saveSceneAs(); });
    menu.addItem("Load Scene", [this] { loadScene(true); });
    menu.addItem("Open Scenes Folder", [this] {
        const auto dir = SceneBridge::getSceneFile().getParentDirectory();
        dir.createDirectory();
        dir.startAsProcess();
    });
    menu.addSeparator();
    menu.addItem("Reset to Defaults", [this] {
        if (audioEngine.isOpeningDevices()) {
            showError("Audio is still starting up. Please try again in a moment.");
            return;
        }
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
                                            "PluginRack",
                                            "Reset engine to defaults (1 channel, 3 outputs)?",
                                            "Reset", "Cancel", nullptr,
                                            juce::ModalCallbackFunction::create([this](int result) {
                                                if (result != 1) return;
                                                // The rack may have docked an editor owned by a strip
                                                // removed below: release it while the processors are
                                                // still alive. Release-only on purpose (see InsertHost.h).
                                                if (auto release = pluginRemovingHook()) release();
                                                // Mixer strips hold Channel&/Output& refs and their open
                                                // EQ windows call back into those refs on destruction
                                                // (analyzerTap): close them while the engine strips are
                                                // still alive, before any removeChannel/removeOutput frees
                                                // the refs the pending rebuild would otherwise tear down.
                                                mixer.closeAllEqWindows();
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
                                                // Truncate keeps the first channel + first 3 outputs, so
                                                // their inserts survive the reset: clear inserts and
                                                // restore default bypassed EQ, then reset the survivors
                                                // themselves to defaults (gains, trim, pan, mutes, solos,
                                                // layouts, routes). Names and devices are kept.
                                                for (const auto& c : audioEngine.getChannels()) {
                                                    std::vector<std::uint32_t> insertIds;
                                                    for (const auto& insert : c->inserts)
                                                        insertIds.push_back(insert->id);
                                                    for (const auto insertId : insertIds)
                                                        audioEngine.removePlugin(c->id, insertId);
                                                    audioEngine.setChannelEq(c->id, pluginrack::defaultEqBands());
                                                    audioEngine.setChannelEqBypassed(c->id, true);
                                                    audioEngine.setChannelEqDelay(c->id, 0.0f);
                                                    audioEngine.setChannelGain(c->id, 0.0f);
                                                    audioEngine.setChannelTrim(c->id, 0.0f);
                                                    audioEngine.setChannelPan(c->id, 0.0f);
                                                    audioEngine.setChannelMuted(c->id, false);
                                                    audioEngine.setChannelSoloed(c->id, false);
                                                    audioEngine.setChannelMapping(c->id, pluginrack::ChannelLayout::Stereo, {0, 1});
                                                }
                                                for (std::size_t i = 0; i < audioEngine.getOutputs().size(); ++i) {
                                                    const auto outputId = audioEngine.getOutputs()[i]->id;
                                                    std::vector<std::uint32_t> insertIds;
                                                    for (const auto& insert : audioEngine.getOutputs()[i]->inserts)
                                                        insertIds.push_back(insert->id);
                                                    for (const auto insertId : insertIds)
                                                        audioEngine.removeOutputPlugin(outputId, insertId);
                                                    audioEngine.setOutputEq((int)i, pluginrack::defaultEqBands());
                                                    audioEngine.setOutputEqBypassed((int)i, true);
                                                    audioEngine.setOutputEqDelay((int)i, 0.0f);
                                                    audioEngine.setOutputGain(outputId, 0.0f);
                                                    audioEngine.setOutputMuted(outputId, false);
                                                    audioEngine.setOutputLayout(outputId, pluginrack::ChannelLayout::Stereo);
                                                    audioEngine.setOutputFlipped(outputId, false);
                                                }
                                                // Default routing: the channel is hot (0 dB) to the first
                                                // output only, silent (-60 dB) elsewhere, so no stale Sub
                                                // send survives the reset.
                                                if (!audioEngine.getChannels().empty() && !audioEngine.getOutputs().empty()) {
                                                    const auto firstOutId = audioEngine.getOutputs().front()->id;
                                                    for (const auto& c : audioEngine.getChannels()) {
                                                        for (const auto& o : audioEngine.getOutputs()) {
                                                            audioEngine.setSendGain(c->id, o->id,
                                                                o->id == firstOutId ? 0.0f : -60.0f);
                                                        }
                                                    }
                                                }
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
        menu.addItem("Output FIFO " + ms(report.fifoMs), false, false, [] {});
        if (report.measuredValid) {
            menu.addItem("Measured round-trip " + ms(report.measuredMs), false, false, [] {});
        } else if (report.measureFailed) {
            menu.addItem("Measurement failed", false, false, [] {});
        }
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
    for (const auto& [stripName, stripError] : audioEngine.getDeviceErrors())
        lines.add(stripName + ": " + stripError);
    if (lines.isEmpty()) lines.add("No audio errors.");
    showError(lines.joinIntoString("\n"));
}

void MainComponent::showSnapshotPlaceholder() {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "PluginRack",
                                           "Snapshots are not implemented yet - scene save/load stays in the toolbar.");
}

void MainComponent::saveScene() {
    const auto error = SceneBridge::save(audioEngine.makeScene());
    if (error.isNotEmpty()) showError(error);
}

void MainComponent::saveSceneAs() {
    auto* w = new juce::AlertWindow("Save Scene", "Enter a name for this save.", juce::MessageBoxIconType::QuestionIcon);
    w->addTextEditor("name", "", "Save name");
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w](int result) {
        if (result == 1 && safe != nullptr) {
            const auto name = w->getTextEditorContents("name");
            const auto error = SceneBridge::saveNamed(safe->audioEngine.makeScene(), name);
            if (error.isNotEmpty()) safe->showError(error);
            else safe->refreshViews();
        }
        delete w;
    }), true);
}

void MainComponent::showSavesMenu() {
    juce::PopupMenu menu;
    menu.addItem("Save As...", [this] { saveSceneAs(); });

    const auto names = SceneBridge::getSaveNames();
    if (!names.isEmpty()) {
        juce::PopupMenu loadMenu;
        for (const auto& name : names) {
            loadMenu.addItem(name, [this, name] {
                juce::String error;
                const auto saved = SceneBridge::loadNamed(name, error);
                if (error.isNotEmpty()) { showError(error); return; }
                if (!saved.has_value()) return;
                const auto pluginErrors = applyScene(*saved);
                if (pluginErrors.isNotEmpty())
                    showError("The scene was loaded, but some plugins could not be restored:\n" + pluginErrors);
            });
        }
        menu.addSubMenu("Load Save", loadMenu);

        juce::PopupMenu defaultMenu;
        for (const auto& name : names) {
            defaultMenu.addItem(name, [name] { SceneBridge::setDefaultSaveName(name); });
        }
        defaultMenu.addItem("Clear Default", [] { SceneBridge::setDefaultSaveName({}); });
        menu.addSubMenu("Set Default Save", defaultMenu);

        juce::PopupMenu deleteMenu;
        for (const auto& name : names) {
            deleteMenu.addItem(name, [this, name] {
                juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "PluginRack",
                    "Delete save '" + name + "'?", "Delete", "Cancel", nullptr,
                    juce::ModalCallbackFunction::create([name](int result) {
                        if (result == 1) SceneBridge::deleteSave(name);
                    }));
            });
        }
        menu.addSubMenu("Delete Save", deleteMenu);

        juce::PopupMenu renameMenu;
        for (const auto& name : names) {
            renameMenu.addItem(name, [this, name] {
                auto* w = new juce::AlertWindow("Rename Save", "New name for '" + name + "'.", juce::MessageBoxIconType::QuestionIcon);
                w->addTextEditor("name", name, "New name");
                w->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
                w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                juce::Component::SafePointer<MainComponent> safe(this);
                w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w, name](int result) {
                    if (result == 1 && safe != nullptr) {
                        const auto error = SceneBridge::renameSave(name, w->getTextEditorContents("name"));
                        if (error.isNotEmpty()) safe->showError(error);
                    }
                    delete w;
                }), true);
            });
        }
        menu.addSubMenu("Rename Save", renameMenu);

        juce::PopupMenu moveMenu;
        for (const auto& name : names) {
            moveMenu.addItem(name, [this, name] {
                fileChooser = std::make_unique<juce::FileChooser>("Choose destination folder");
                juce::Component::SafePointer<MainComponent> safe(this);
                fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                    [safe, name](const juce::FileChooser& chooser) {
                        if (safe == nullptr) return;
                        const auto result = chooser.getResult();
                        if (result == juce::File()) return;
                        const auto error = SceneBridge::moveSave(name, result);
                        if (error.isNotEmpty()) safe->showError(error);
                    });
            });
        }
        menu.addSubMenu("Move Save", moveMenu);
    } else {
        menu.addItem("No saves yet", false, false, [] {});
    }

    menu.addSeparator();
    menu.addItem("Open Saves Folder", [this] {
        const auto dir = SceneBridge::getScenesDirectory();
        dir.createDirectory();
        dir.startAsProcess();
    });

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(toolbar.getMenuTarget()));
}

void MainComponent::loadScene(bool showMissingMessage) {
    juce::String error;
    auto saved = SceneBridge::loadDefaultOrLast(error);
    if (error.isNotEmpty()) { showError(error); return; }
    if (!saved.has_value()) {
        if (showMissingMessage) showError("No scene has been saved yet.");
        return;
    }
    const auto pluginErrors = applyScene(*saved);
    if (pluginErrors.isNotEmpty())
        showError("The scene was loaded, but some plugins could not be restored:\n" + pluginErrors);
}

juce::String MainComponent::applyScene(const pluginrack::Scene& scene, bool openDevicesNow) {
    // All of restoreScene runs here, on the message thread: it
    // instantiates plugins, swaps the strip vectors, and destroys the old
    // strips whose PluginHosts own JUCE editor windows - none of which may
    // happen on a worker thread. The device streams themselves are opened by
    // the engine's startup worker when openDevicesNow is false. The flag
    // serialises overlapping menu/Saves loads and resets on every path;
    // dialogs stay on the message thread too.
    if (sceneLoading_.exchange(true)) return {};
    if (audioEngine.isOpeningDevices()) {
        sceneLoading_.store(false);
        showError("Audio is still starting up. Please try again in a moment.");
        return {};
    }
    // The rack may dock an editor from a strip whose plugin dies inside
    // restoreScene; release it first (an editor outliving its processor
    // is unsafe to delete). refreshViews below re-docks against the new
    // strips. Mixer strips hold Channel&/Output& refs and their open EQ
    // windows call back into those refs on destruction (analyzerTap): close
    // them while the engine strips are still alive, before restoreScene
    // frees the refs the pending rebuild would otherwise tear down.
    router.releaseDockedEditor();
    mixer.closeAllEqWindows();
    const juce::String pluginErrors = audioEngine.restoreScene(scene, openDevicesNow);
    sceneLoading_.store(false);
    refreshViews();
    return pluginErrors;
}

void MainComponent::initialiseAudio() {
    juce::String error;
    auto saved = SceneBridge::loadDefaultOrLast(error);
    if (error.isNotEmpty()) {
        showError(error);
        audioEngine.openDefaultDevicesAsync();
    } else if (saved.has_value()) {
        // Build the strips and load the plugins now (fast), then let the
        // engine open the streams on its worker: a slow WASAPI probe must not
        // take the message thread with it.
        const auto pluginErrors = applyScene(*saved, /*openDevicesNow=*/false);
        if (pluginErrors.isNotEmpty())
            showError("The scene was loaded, but some plugins could not be restored:\n" + pluginErrors);
        audioEngine.openStripsDevicesAsync();
    } else {
        audioEngine.openDefaultDevicesAsync();
    }
    timerCallback();
}

void MainComponent::saveSceneOnExit() {
    SceneBridge::saveLast(audioEngine.makeScene());
}

void MainComponent::refreshViews() {
    // Belt and braces alongside the applyScene pre-close: never rebuild the
    // mixer while the rack still holds an editor for an insert the engine
    // has freed. Null-guarded, so a second release is a no-op; releasing
    // only deletes the editor and never calls back into the views.
    router.releaseDockedEditor();
    mixer.rebuild();
    router.refreshViews();
}

void MainComponent::showError(const juce::String& message) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", message);
}
