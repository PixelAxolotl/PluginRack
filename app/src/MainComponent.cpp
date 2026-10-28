#include "MainComponent.h"

#include "SceneBridge.h"

MainComponent::MainComponent()
    : mixer(audioEngine),
      routingMatrix(audioEngine),
      deviceSelector(deviceManager, 0, 2, 0, 2, false, false, true, false) {
    setAudioChannels(2, 2);

    const auto tabColour = findColour(juce::ResizableWindow::backgroundColourId);
    tabs.addTab("Mixer", tabColour, &mixer, false);
    tabs.addTab("Routing", tabColour, &routingMatrix, false);
    addAndMakeVisible(tabs);
    addAndMakeVisible(deviceSelector);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(loadButton);

    saveButton.onClick = [this] { saveScene(); };
    loadButton.onClick = [this] { loadScene(true); };
    mixer.onStructureChanged = [this] { routingMatrix.rebuild(); };

    loadScene(false);
    setSize(1000, 700);
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
    g.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized() {
    auto area = getLocalBounds().reduced(12);
    auto toolbar = area.removeFromTop(32);
    saveButton.setBounds(toolbar.removeFromLeft(110));
    toolbar.removeFromLeft(8);
    loadButton.setBounds(toolbar.removeFromLeft(110));
    area.removeFromTop(8);
    deviceSelector.setBounds(area.removeFromBottom(170));
    area.removeFromBottom(8);
    tabs.setBounds(area);
}

void MainComponent::saveScene() {
    const auto error = SceneBridge::save(audioEngine.makeScene());
    if (error.isNotEmpty()) {
        showError(error);
    }
}

void MainComponent::loadScene(bool showMissingMessage) {
    juce::String error;
    const auto saved = SceneBridge::load(error);
    if (error.isNotEmpty()) {
        showError(error);
        return;
    }
    if (!saved.has_value()) {
        if (showMissingMessage) {
            showError("No scene has been saved yet.");
        }
        return;
    }

    const auto pluginErrors = audioEngine.restoreScene(*saved);
    refreshViews();
    if (pluginErrors.isNotEmpty()) {
        showError("The scene was loaded, but some plugins could not be restored:\n" + pluginErrors);
    }
}

void MainComponent::refreshViews() {
    mixer.rebuild();
    routingMatrix.rebuild();
}

void MainComponent::showError(const juce::String& message) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack",
                                           message);
}
