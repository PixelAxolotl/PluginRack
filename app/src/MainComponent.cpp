#include "MainComponent.h"

#include "SceneBridge.h"

MainComponent::PeakMeter::PeakMeter(juce::Atomic<float>& source)
    : levelSource(source) {
    startTimerHz(30);
}

void MainComponent::PeakMeter::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds();
    g.fillAll(juce::Colour(0xff202020));
    g.setColour(juce::Colour(0xff56b870));
    g.fillRect(bounds.removeFromLeft(juce::roundToInt(bounds.getWidth() * displayedLevel)));
    g.setColour(juce::Colours::grey);
    g.drawRect(getLocalBounds());
}

void MainComponent::PeakMeter::timerCallback() {
    const auto newLevel = juce::jlimit(0.0f, 1.0f, levelSource.exchange(0.0f));
    displayedLevel = juce::jmax(newLevel, displayedLevel * 0.85f);
    repaint();
}

MainComponent::MainComponent()
    : levelMeter(outputLevel),
      deviceSelector(deviceManager, 0, 2, 0, 2, false, false, true, false) {
    setAudioChannels(2, 2);

    addAndMakeVisible(deviceSelector);
    addAndMakeVisible(loadButton);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(pluginLabel);
    addAndMakeVisible(levelMeter);

    pluginLabel.setText("No plugin loaded", juce::dontSendNotification);
    loadButton.onClick = [this] { choosePlugin(); };
    saveButton.onClick = [this] { saveScene(); };

    restoreScene();
    setSize(760, 600);
}

MainComponent::~MainComponent() {
    shutdownAudio();
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate) {
    pluginHost.prepareToPlay(sampleRate, samplesPerBlockExpected);
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) {
    auto* buffer = bufferToFill.buffer;
    if (buffer == nullptr || buffer->getNumChannels() == 0 || bufferToFill.numSamples <= 0) {
        return;
    }

    juce::AudioBuffer<float> block(buffer->getArrayOfWritePointers(), buffer->getNumChannels(),
                                   bufferToFill.startSample, bufferToFill.numSamples);
    pluginHost.processBlock(block);
    outputLevel = block.getMagnitude(0, bufferToFill.numSamples);
}

void MainComponent::releaseResources() {
    pluginHost.releaseResources();
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized() {
    auto area = getLocalBounds().reduced(16);
    auto buttons = area.removeFromTop(32);
    loadButton.setBounds(buttons.removeFromLeft(120));
    buttons.removeFromLeft(8);
    saveButton.setBounds(buttons.removeFromLeft(120));

    area.removeFromTop(12);
    pluginLabel.setBounds(area.removeFromTop(26));
    area.removeFromTop(8);
    levelMeter.setBounds(area.removeFromTop(22));
    area.removeFromTop(12);
    deviceSelector.setBounds(area);
}

void MainComponent::choosePlugin() {
    auto initialLocation = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    if (const auto path = pluginHost.getPluginPath(); path.isNotEmpty()) {
        initialLocation = juce::File(path).getParentDirectory();
    }

    fileChooser = std::make_unique<juce::FileChooser>("Select a VST3 plugin", initialLocation,
                                                       "*.vst3");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& chooser) {
                                 const auto file = chooser.getResult();
                                 if (file != juce::File()) {
                                     loadPluginFile(file, true);
                                 }
                             });
}

void MainComponent::loadPluginFile(const juce::File& file, bool showErrors) {
    const auto error = pluginHost.loadPlugin(file);
    if (error.isNotEmpty()) {
        if (showErrors) {
            showError(error);
        } else {
            pluginLabel.setText("Saved plugin could not be loaded", juce::dontSendNotification);
        }
        return;
    }

    pluginLabel.setText("Loaded: " + pluginHost.getPluginName(), juce::dontSendNotification);
}

void MainComponent::saveScene() {
    const auto error = SceneBridge::save(pluginHost.getPluginPath(), pluginHost.getPluginName(),
                                         pluginHost.getSampleRate(), pluginHost.getBufferSize());
    if (error.isNotEmpty()) {
        showError(error);
    }
}

void MainComponent::restoreScene() {
    juce::String error;
    const auto saved = SceneBridge::load(error);
    if (error.isNotEmpty()) {
        pluginLabel.setText(error, juce::dontSendNotification);
        return;
    }
    if (saved.has_value() && saved->pluginPath.isNotEmpty()) {
        loadPluginFile(juce::File(saved->pluginPath), false);
    }
}

void MainComponent::showError(const juce::String& message) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack",
                                           message);
}
