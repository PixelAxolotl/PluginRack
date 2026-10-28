#include "MainComponent.h"

#include "SceneBridge.h"

MainComponent::MainComponent()
    : mixer(audioEngine),
      deviceSelector(deviceManager, 0, 2, 0, 2, false, false, true, false) {
    setAudioChannels(2, 2);

    addAndMakeVisible(mixer);
    addAndMakeVisible(deviceSelector);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(loadButton);
    addAndMakeVisible(menuButton);

    saveButton.onClick = [this] { saveScene(); };
    loadButton.onClick = [this] { loadScene(true); };

    mixer.onStructureChanged = [this] { /* no-op */ };

    loadScene(false);
    setSize(1600, 900);
}

MainComponent::~MainComponent() {
    shutdownAudio();
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate) {
    audioEngine.prepareToPlay(samplesPerBlockExpected, sampleRate);
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) {
    audioEngine.processBlock(bufferToFill);
}

void MainComponent::releaseResources() {
    audioEngine.releaseResources();
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

    // hardware out + sample rate (right side, before menu)
    double sr = 48000.0;
    if (auto* dev = deviceManager.getCurrentAudioDevice()) sr = dev->getCurrentSampleRate();
    juce::String info = "Hardware Out: Default    SR: " + juce::String((int)sr) + " Hz";
    g.setColour(theme::dim());
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText(info, getWidth() - 420, 0, 260, 56, juce::Justification::centredRight);
}

void MainComponent::resized() {
    auto area = getLocalBounds();
    auto header = area.removeFromTop(56);

    // header buttons right
    auto hr = header.reduced(12, 12);
    hr.removeFromLeft(220); // wordmark space
    // right side buttons
    menuButton.setBounds(hr.removeFromRight(80));
    hr.removeFromRight(8);
    loadButton.setBounds(hr.removeFromRight(60));
    hr.removeFromRight(4);
    saveButton.setBounds(hr.removeFromRight(60));

    area.removeFromTop(0);
    // device selector bottom
    deviceSelector.setBounds(area.removeFromBottom(90));
    area.removeFromBottom(4);
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
