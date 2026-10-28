#include "RoutingMatrixComponent.h"

namespace {
constexpr int rowHeaderWidth = 130;
constexpr int columnWidth = 160;
constexpr int headerHeight = 36;
constexpr int rowHeight = 52;
}

RoutingMatrixComponent::RoutingMatrixComponent(AudioEngine& engine)
    : audioEngine(engine) {
    gridViewport.setViewedComponent(&gridContent, false);
    addAndMakeVisible(gridViewport);
    rebuild();
}

void RoutingMatrixComponent::resized() {
    gridViewport.setBounds(getLocalBounds().reduced(8));
    const int requiredWidth = rowHeaderWidth
                              + static_cast<int>(audioEngine.getOutputs().size()) * columnWidth;
    const int requiredHeight = headerHeight
                               + static_cast<int>(audioEngine.getChannels().size()) * rowHeight;
    gridContent.setSize(juce::jmax(gridViewport.getWidth(), requiredWidth),
                        juce::jmax(gridViewport.getHeight(), requiredHeight));

    for (int output = 0; output < outputHeaders.size(); ++output) {
        outputHeaders[output]->setBounds(rowHeaderWidth + output * columnWidth, 0,
                                         columnWidth - 8, headerHeight);
    }
    for (int channel = 0; channel < channelLabels.size(); ++channel) {
        channelLabels[channel]->setBounds(0, headerHeight + channel * rowHeight,
                                          rowHeaderWidth - 8, rowHeight);
    }

    int cell = 0;
    for (int channel = 0; channel < channelLabels.size(); ++channel) {
        for (int output = 0; output < outputHeaders.size(); ++output) {
            if (cell < sendSliders.size()) {
                sendSliders[cell]->setBounds(rowHeaderWidth + output * columnWidth,
                                             headerHeight + channel * rowHeight + 8,
                                             columnWidth - 12, 34);
            }
            ++cell;
        }
    }
}

void RoutingMatrixComponent::rebuild() {
    outputHeaders.clear();
    channelLabels.clear();
    sendSliders.clear();

    for (const auto& output : audioEngine.getOutputs()) {
        auto* header = outputHeaders.add(new juce::Label());
        header->setText(output->name, juce::dontSendNotification);
        header->setJustificationType(juce::Justification::centred);
        header->setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        gridContent.addAndMakeVisible(*header);
    }
    for (const auto& channel : audioEngine.getChannels()) {
        auto* label = channelLabels.add(new juce::Label());
        label->setText(channel->name, juce::dontSendNotification);
        label->setJustificationType(juce::Justification::centredLeft);
        gridContent.addAndMakeVisible(*label);

        for (const auto& output : audioEngine.getOutputs()) {
            auto* slider = sendSliders.add(new juce::Slider());
            slider->setSliderStyle(juce::Slider::LinearBar);
            slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 24);
            slider->setRange(-60.0, 12.0, 0.1);
            slider->setTextValueSuffix(" dB");
            slider->setDoubleClickReturnValue(true, 0.0);
            slider->setValue(audioEngine.getSendGain(channel->id, output->id),
                             juce::dontSendNotification);
            const auto channelId = channel->id;
            const auto outputId = output->id;
            slider->onValueChange = [this, slider, channelId, outputId] {
                audioEngine.setSendGain(channelId, outputId,
                                        static_cast<float>(slider->getValue()));
            };
            gridContent.addAndMakeVisible(*slider);
        }
    }
    resized();
}
