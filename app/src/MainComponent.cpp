#include "MainComponent.h"

#include <vector>

#include "SceneBridge.h"

MainComponent::MainComponent()
    : mixer(audioEngine) {
    addAndMakeVisible(mixer);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(loadButton);
    addAndMakeVisible(menuButton);

    latencyLabel.setJustificationType(juce::Justification::centredRight);
    latencyLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    latencyLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(latencyLabel);

    saveButton.onClick = [this] { saveScene(); };
    loadButton.onClick = [this] { loadScene(true); };

    menuButton.onClick = [this] {
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

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(menuButton));
    };

    mixer.onStructureChanged = [this] { /* no-op */ };

    loadScene(false);
    startTimerHz(4);
    timerCallback(); // populate the latency readout before the first tick
    setSize(1600, 900);
}

void MainComponent::timerCallback() {
    const auto report = audioEngine.getLatencyReport();
    if (!report.valid) {
        smoothedLatencyMs = -1.0;
        latencyLabel.setText({}, juce::dontSendNotification);
        latencyLabel.setTooltip({});
        return;
    }
    // A live glitch counter overrides smoothing: snap to the raw total so
    // the EMA never hides an active underrun/overrun.
    const bool glitch = (report.underruns + report.overruns) > 0;
    if (glitch || smoothedLatencyMs < 0.0) smoothedLatencyMs = report.totalMs;
    else smoothedLatencyMs += 0.25 * (report.totalMs - smoothedLatencyMs);
    juce::String latText = "LAT ~" + juce::String((int)std::round(smoothedLatencyMs)) + " ms (est.)";
    if (glitch) latText += " !";
    latencyLabel.setText(latText, juce::dontSendNotification);
    juce::Colour colour = theme::dim();
    if (glitch) colour = theme::red();
    else if (smoothedLatencyMs > 60.0) colour = theme::red();
    else if (smoothedLatencyMs > 30.0) colour = theme::amber();
    latencyLabel.setColour(juce::Label::textColourId, colour);
    latencyLabel.setTooltip("Estimated (approx.) input to output: input " +
                            juce::String((int)std::round(report.inputDeviceMs)) + " ms (buffer " +
                            juce::String(report.inputBufferSize) + " smp) + plugins " +
                            juce::String((int)std::round(report.pluginMs)) + " ms + output " +
                            juce::String((int)std::round(report.outputDeviceMs)) + " ms (buffer " +
                            juce::String(report.outputBufferSize) + " smp) + FIFO " +
                            juce::String((int)std::round(report.fifoMs)) + " ms" +
                            (glitch ? " - dropouts detected" : ""));
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());

    // header bar
    auto header = getLocalBounds().removeFromTop(56);
    g.setColour(theme::panel());
    g.fillRect(header);
    g.setColour(theme::border());
    g.drawHorizontalLine(header.getBottom() - 1, 0.0f, (float)getWidth());

    // wordmark
    g.setColour(theme::green());
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("PluginRack", 20, 0, 200, 56, juce::Justification::centredLeft);

    // clock info
    const auto desc = audioEngine.getClockDescription();
    const bool noClock = desc.startsWith("No output");
    g.setColour(noClock ? theme::amber() : theme::dim());
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    const int clockRight = latencyLabel.getX() - 8;
    g.drawFittedText(desc, 220, 0, juce::jmax(0, clockRight - 220), 56, juce::Justification::centredRight, 1);
}

void MainComponent::resized() {
    auto area = getLocalBounds();
    auto header = area.removeFromTop(56);

    // header buttons right
    auto hr = header.reduced(12, 12);
    hr.removeFromLeft(220); // wordmark space
    menuButton.setBounds(hr.removeFromRight(80));
    hr.removeFromRight(8);
    loadButton.setBounds(hr.removeFromRight(60));
    hr.removeFromRight(4);
    saveButton.setBounds(hr.removeFromRight(60));
    hr.removeFromRight(8);
    latencyLabel.setBounds(hr.removeFromRight(140));

    mixer.setBounds(area);
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
}

void MainComponent::showError(const juce::String& message) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", message);
}
