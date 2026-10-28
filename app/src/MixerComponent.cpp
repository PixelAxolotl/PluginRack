#include "MixerComponent.h"

MixerComponent::OutputStrip::OutputStrip(AudioEngine::Output& outputState, AudioEngine& engine)
    : output(outputState), audioEngine(engine), meter(outputState.peak) {
    nameLabel.setText(output.name, juce::dontSendNotification);
    nameLabel.setJustificationType(juce::Justification::centred);
    nameLabel.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    nameLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(nameLabel);

    gainSlider.setSliderStyle(juce::Slider::LinearVertical);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxAbove, false, 40, 14);
    gainSlider.setRange(-60.0, 12.0, 0.1);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setDoubleClickReturnValue(true, 0.0);
    gainSlider.setValue(output.gainDb.get(), juce::dontSendNotification);
    gainSlider.onValueChange = [this] {
        audioEngine.setOutputGain(output.id, static_cast<float>(gainSlider.getValue()));
    };
    gainSlider.setColour(juce::Slider::trackColourId, theme::deepGreen());
    gainSlider.setColour(juce::Slider::backgroundColourId, theme::strip());
    gainSlider.setColour(juce::Slider::thumbColourId, theme::green());
    gainSlider.setColour(juce::Slider::textBoxTextColourId, theme::text());
    gainSlider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    addAndMakeVisible(gainSlider);

    addAndMakeVisible(meter);

    muteButton.setClickingTogglesState(true);
    muteButton.setToggleState(output.muted.get(), juce::dontSendNotification);
    muteButton.setColour(juce::TextButton::buttonColourId, theme::control());
    muteButton.setColour(juce::TextButton::buttonOnColourId, theme::red());
    muteButton.setColour(juce::TextButton::textColourOffId, theme::text());
    muteButton.onClick = [this] {
        audioEngine.setOutputMuted(output.id, muteButton.getToggleState());
    };
    addAndMakeVisible(muteButton);
}

void MixerComponent::OutputStrip::resized() {
    auto area = getLocalBounds().reduced(4);
    nameLabel.setBounds(area.removeFromTop(18));
    auto mb = area.removeFromBottom(22);
    muteButton.setBounds(mb);
    area.removeFromBottom(4);
    // meter right
    auto meterArea = area.removeFromRight(12);
    area.removeFromRight(4);
    meter.setBounds(meterArea);
    gainSlider.setBounds(area);
}

void MixerComponent::OutputStrip::refresh() {
    gainSlider.setValue(output.gainDb.get(), juce::dontSendNotification);
    muteButton.setToggleState(output.muted.get(), juce::dontSendNotification);
}

MixerComponent::MixerComponent(AudioEngine& engine)
    : audioEngine(engine) {
    addChannelButton.onClick = [this] {
        audioEngine.addChannel();
        rebuild();
        if (onStructureChanged) onStructureChanged();
    };
    addChannelButton.setColour(juce::TextButton::buttonColourId, theme::control());
    addChannelButton.setColour(juce::TextButton::textColourOffId, theme::text());
    addAndMakeVisible(addChannelButton);

    addOutputButton.onClick = [this] {
        audioEngine.addOutput();
        rebuild();
        if (onStructureChanged) onStructureChanged();
    };
    addOutputButton.setColour(juce::TextButton::buttonColourId, theme::control());
    addOutputButton.setColour(juce::TextButton::textColourOffId, theme::text());
    addAndMakeVisible(addOutputButton);

    channelViewport.setViewedComponent(&stripContent, false);
    channelViewport.setScrollBarsShown(false, true);
    addAndMakeVisible(channelViewport);

    masterHeader.setJustificationType(juce::Justification::centred);
    masterHeader.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    masterHeader.setColour(juce::Label::textColourId, theme::amber());
    masterPanel.addAndMakeVisible(masterHeader);

    masterViewport.setViewedComponent(&masterContent, false);
    masterViewport.setScrollBarsShown(true, false);
    masterPanel.addAndMakeVisible(masterViewport);
    addAndMakeVisible(masterPanel);

    rebuild();
}

void MixerComponent::resized() {
    auto area = getLocalBounds().reduced(4);
    auto toolbar = area.removeFromTop(28);
    addChannelButton.setBounds(toolbar.removeFromLeft(60));
    toolbar.removeFromLeft(4);
    addOutputButton.setBounds(toolbar.removeFromLeft(60));
    area.removeFromTop(4);

    const int masterW = 360;
    masterPanel.setBounds(area.removeFromRight(masterW));
    area.removeFromRight(6);
    channelViewport.setBounds(area);

    // strip content size
    const int stripW = 148 + 6;
    const int contentW = juce::jmax(area.getWidth(), (int)strips.size() * stripW + 8);
    stripContent.setSize(contentW, area.getHeight());
    for (int i = 0; i < (int)strips.size(); ++i) {
        strips[i]->setBounds(i * stripW + 4, 0, 148, area.getHeight());
    }

    // master content
    auto ma = masterPanel.getLocalBounds();
    masterHeader.setBounds(ma.removeFromTop(24));
    masterViewport.setBounds(ma);
    const int outW = 60;
    const int outGap = 4;
    const int contentW2 = (int)outputStrips.size() * (outW + outGap) + 8;
    masterContent.setSize(juce::jmax(masterViewport.getWidth(), contentW2), masterViewport.getHeight());
    for (int i = 0; i < (int)outputStrips.size(); ++i) {
        outputStrips[i]->setBounds(i * (outW + outGap) + 4, 0, outW, masterViewport.getHeight());
    }
}

void MixerComponent::rebuild() {
    strips.clear();
    for (const auto& ch : audioEngine.getChannels()) {
        auto* s = strips.add(new ChannelStrip(*ch, audioEngine));
        stripContent.addAndMakeVisible(*s);
    }

    outputStrips.clear();
    for (const auto& out : audioEngine.getOutputs()) {
        auto* s = outputStrips.add(new OutputStrip(*out, audioEngine));
        masterContent.addAndMakeVisible(*s);
    }
    resized();
}
