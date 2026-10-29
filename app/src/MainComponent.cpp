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
        const auto report = audioEngine.getLatencyReport();
        juce::String latText;
        if (report.valid) {
            latText = "Input " + juce::String((int)std::round(report.inputDeviceMs)) + " ms / Plugins " +
                      juce::String((int)std::round(report.pluginMs)) + " ms / Output " +
                      juce::String((int)std::round(report.outputDeviceMs)) + " ms / Total " +
                      juce::String((int)std::round(report.totalMs)) + " ms (est.)";
        } else {
            latText = "No output device";
        }
        menu.addItem(latText, false, false, [] {});

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
    // Smooth the readout: the FIFO queue-depth term is instantaneous and
    // spikes during startup/transients. EMA keeps the header stable live.
    if (smoothedLatencyMs < 0.0) smoothedLatencyMs = report.totalMs;
    else smoothedLatencyMs += 0.25 * (report.totalMs - smoothedLatencyMs);
    latencyLabel.setText("LAT ~" + juce::String((int)std::round(smoothedLatencyMs)) + " ms (est.)",
                         juce::dontSendNotification);
    juce::Colour colour = theme::dim();
    if (smoothedLatencyMs > 60.0) colour = theme::red();
    else if (smoothedLatencyMs > 30.0) colour = theme::amber();
    latencyLabel.setColour(juce::Label::textColourId, colour);
    latencyLabel.setTooltip("Estimated (approx.) input to output: input " +
                            juce::String((int)std::round(report.inputDeviceMs)) + " ms + plugins " +
                            juce::String((int)std::round(report.pluginMs)) + " ms + output " +
                            juce::String((int)std::round(report.outputDeviceMs)) + " ms");
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
    latencyLabel.setBounds(hr.removeFromRight(90));

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
