#include "RoutingMatrixComponent.h"

#include "Theme.h"

namespace {
constexpr int rowHeaderWidth = 130;
constexpr int columnWidth = 200;
constexpr int headerHeight = 36;
constexpr int rowHeight = 72;
}

RoutingMatrixComponent::RoutingMatrixComponent(AudioEngine& engine)
    : audioEngine(engine) {
    addAndMakeVisible(header);
    gridViewport.setViewedComponent(&gridContent, false);
    addAndMakeVisible(gridViewport);
    rebuild();
}

void RoutingMatrixComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
}

void RoutingMatrixComponent::resized() {
    auto area = getLocalBounds().reduced(theme::ui::kPagePad);
    header.setBounds(area.removeFromTop(theme::ui::kHeaderH));
    area.removeFromTop(theme::ui::kHeaderGap);
    gridViewport.setBounds(area);

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
            if (cell * 2 + 1 < sendSliders.size()) {
                const int x = rowHeaderWidth + output * columnWidth;
                const int y = headerHeight + channel * rowHeight;
                sendSideLabels[cell * 2]->setBounds(x + 2, y + 4, 18, 28);
                sendSliders[cell * 2]->setBounds(x + 20, y + 4, columnWidth - 32, 28);
                sendSideLabels[cell * 2 + 1]->setBounds(x + 2, y + 36, 18, 28);
                sendSliders[cell * 2 + 1]->setBounds(x + 20, y + 36, columnWidth - 32, 28);
            }
            ++cell;
        }
    }
}

void RoutingMatrixComponent::rebuild() {
    outputHeaders.clear();
    channelLabels.clear();
    sendSideLabels.clear();
    sendSliders.clear();

    const int inputCount = (int)audioEngine.getChannels().size();
    const int outputCount = (int)audioEngine.getOutputs().size();
    header.setStatus(juce::String(inputCount) + (inputCount == 1 ? " input" : " inputs")
                     + "  -  " + juce::String(outputCount)
                     + (outputCount == 1 ? " output" : " outputs"));

    for (const auto& output : audioEngine.getOutputs()) {
        auto* headerLabel = outputHeaders.add(new juce::Label());
        headerLabel->setText(output->name, juce::dontSendNotification);
        headerLabel->setJustificationType(juce::Justification::centred);
        headerLabel->setFont(theme::ui::itemFont());
        headerLabel->setColour(juce::Label::textColourId, theme::text());
        headerLabel->setColour(juce::Label::backgroundColourId, theme::panel());
        headerLabel->setColour(juce::Label::outlineColourId, theme::border());
        gridContent.addAndMakeVisible(*headerLabel);
    }

    // One L/R pair per cell; the two bars are otherwise indistinguishable.
    const auto addSideLabel = [this](const juce::String& text) {
        auto* label = sendSideLabels.add(new juce::Label());
        label->setText(text, juce::dontSendNotification);
        label->setJustificationType(juce::Justification::centred);
        label->setFont(theme::ui::sectionFont());
        label->setColour(juce::Label::textColourId, theme::dim());
        label->setInterceptsMouseClicks(false, false);
        gridContent.addAndMakeVisible(*label);
    };

    for (const auto& channel : audioEngine.getChannels()) {
        auto* label = channelLabels.add(new juce::Label());
        label->setText(channel->name, juce::dontSendNotification);
        label->setJustificationType(juce::Justification::centredLeft);
        label->setFont(theme::ui::bodyBoldFont());
        label->setColour(juce::Label::textColourId, theme::text());
        label->setColour(juce::Label::backgroundColourId, theme::panel());
        gridContent.addAndMakeVisible(*label);

        for (const auto& output : audioEngine.getOutputs()) {
            const auto addSend = [&](bool rightSide) {
                addSideLabel(rightSide ? "R" : "L");
                auto* slider = sendSliders.add(new ValueEditSlider());
                slider->setName("Send " + channel->name + " > " + output->name);
                slider->setSliderStyle(juce::Slider::LinearBar);
                slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 20);
                slider->setRange(-60.0, 12.0, 0.1);
                slider->setTextValueSuffix(" dB");
                slider->setColour(juce::Slider::trackColourId, theme::cyan());
                slider->setColour(juce::Slider::backgroundColourId, theme::control());
                slider->setColour(juce::Slider::textBoxTextColourId, theme::text());
                slider->setColour(juce::Slider::textBoxBackgroundColourId, theme::panel());
                slider->setColour(juce::Slider::textBoxOutlineColourId, theme::border());
                slider->setDoubleClickReturnValue(true, 0.0);
                slider->setValue(rightSide ? audioEngine.getSendGainR(channel->id, output->id)
                                           : audioEngine.getSendGainL(channel->id, output->id),
                                  juce::dontSendNotification);
                const auto channelId = channel->id;
                const auto outputId = output->id;
                slider->onValueChange = [this, slider, channelId, outputId, rightSide] {
                    if (rightSide)
                        audioEngine.setSendGainR(channelId, outputId,
                                                 static_cast<float>(slider->getValue()));
                    else
                        audioEngine.setSendGainL(channelId, outputId,
                                                 static_cast<float>(slider->getValue()));
                };
                gridContent.addAndMakeVisible(*slider);
            };
            addSend(false);
            addSend(true);
        }
    }
    resized();
}