#include "MixerComponent.h"

MixerComponent::MixerComponent(AudioEngine& engine)
    : audioEngine(engine) {
    addChannelButton.onClick = [this] {
        audioEngine.addChannel();
        rebuild();
        if (onStructureChanged) {
            onStructureChanged();
        }
    };
    addAndMakeVisible(addChannelButton);

    addOutputButton.onClick = [this] {
        audioEngine.addOutput();
        rebuild();
        if (onStructureChanged) {
            onStructureChanged();
        }
    };
    addAndMakeVisible(addOutputButton);

    channelViewport.setViewedComponent(&stripContent, false);
    channelViewport.setScrollBarsShown(false, true);
    addAndMakeVisible(channelViewport);
    addAndMakeVisible(outputsPanel);
    outputsHeader.setJustificationType(juce::Justification::centred);
    outputsHeader.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    outputsPanel.addAndMakeVisible(outputsHeader);
    rebuild();
}

void MixerComponent::resized() {
    auto area = getLocalBounds().reduced(8);
    auto toolbar = area.removeFromTop(32);
    addChannelButton.setBounds(toolbar.removeFromLeft(120));
    toolbar.removeFromLeft(8);
    addOutputButton.setBounds(toolbar.removeFromLeft(120));
    area.removeFromTop(8);

    outputsPanel.setBounds(area.removeFromRight(150));
    area.removeFromRight(8);
    channelViewport.setBounds(area);
    stripContent.setSize(juce::jmax(area.getWidth(), strips.size() * 196), area.getHeight());
    for (int index = 0; index < strips.size(); ++index) {
        strips[index]->setBounds(index * 196, 0, 190, area.getHeight());
    }

    auto outputArea = outputsPanel.getLocalBounds();
    outputsHeader.setBounds(outputArea.removeFromTop(30));
    for (auto* label : outputLabels) {
        label->setBounds(outputArea.removeFromTop(28).reduced(4, 0));
    }
}

void MixerComponent::rebuild() {
    strips.clear();
    for (const auto& channel : audioEngine.getChannels()) {
        auto* strip = strips.add(new ChannelStrip(*channel, audioEngine));
        stripContent.addAndMakeVisible(*strip);
    }

    outputLabels.clear();
    for (const auto& output : audioEngine.getOutputs()) {
        auto* label = outputLabels.add(new juce::Label());
        label->setText(output->name, juce::dontSendNotification);
        label->setJustificationType(juce::Justification::centredLeft);
        label->setColour(juce::Label::backgroundColourId, juce::Colour(0xff303030));
        outputsPanel.addAndMakeVisible(*label);
    }
    resized();
}
