#include "RoutingMatrixComponent.h"

#include "Theme.h"

namespace {
constexpr int rowHeaderWidth = 130;
constexpr int columnWidth = 200;
constexpr int bandHeight = 18;
constexpr int headerHeight = 54;
constexpr int rowHeight = 72;
}

RoutingMatrixComponent::RoutingMatrixComponent(AudioEngine& engine)
    : audioEngine(engine) {
    addAndMakeVisible(header);
    gridViewport.setViewedComponent(&gridContent, false);
    addAndMakeVisible(gridViewport);

    for (auto* band : { &busBand, &outputBand }) {
        band->setJustificationType(juce::Justification::centredLeft);
        band->setFont(theme::ui::sectionFont());
        band->setColour(juce::Label::textColourId, theme::dim());
        band->setInterceptsMouseClicks(false, false);
        gridContent.addAndMakeVisible(*band);
    }
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

    const int columnCount = static_cast<int>(audioEngine.getBuses().size())
                            + static_cast<int>(audioEngine.getOutputs().size());
    const int rowCount = static_cast<int>(audioEngine.getChannels().size())
                         + static_cast<int>(audioEngine.getBuses().size());
    const int requiredWidth = rowHeaderWidth + columnCount * columnWidth;
    const int requiredHeight = headerHeight + rowCount * rowHeight;
    gridContent.setSize(juce::jmax(gridViewport.getWidth(), requiredWidth),
                        juce::jmax(gridViewport.getHeight(), requiredHeight));

    busBand.setVisible(busColumnCount > 0);
    outputBand.setVisible(columnCount > busColumnCount);
    const int outputColumnCount = columnCount - busColumnCount;
    busBand.setBounds(rowHeaderWidth, 0, busColumnCount * columnWidth, bandHeight);
    outputBand.setBounds(rowHeaderWidth + busColumnCount * columnWidth, 0,
                         outputColumnCount * columnWidth, bandHeight);

    for (int column = 0; column < columnHeaders.size(); ++column) {
        columnHeaders[column]->setBounds(rowHeaderWidth + column * columnWidth, bandHeight,
                                         columnWidth - 8, headerHeight - bandHeight);
    }
    for (int row = 0; row < rowLabels.size(); ++row) {
        rowLabels[row]->setBounds(0, headerHeight + row * rowHeight,
                                  rowHeaderWidth - 8, rowHeight);
    }

    for (const Cell& cell : cells) {
        const int x = rowHeaderWidth + cell.column * columnWidth;
        const int y = headerHeight + cell.row * rowHeight;
        cell.sideL->setBounds(x + 2, y + 4, 18, 28);
        cell.sliderL->setBounds(x + 20, y + 4, columnWidth - 32, 28);
        cell.sideR->setBounds(x + 2, y + 36, 18, 28);
        cell.sliderR->setBounds(x + 20, y + 36, columnWidth - 32, 28);
    }
}

void RoutingMatrixComponent::rebuild() {
    columnHeaders.clear();
    rowLabels.clear();
    sendSideLabels.clear();
    sendSliders.clear();
    cells.clear();

    const auto& channels = audioEngine.getChannels();
    const auto& buses = audioEngine.getBuses();
    const auto& outputs = audioEngine.getOutputs();
    busColumnCount = static_cast<int>(buses.size());
    channelRowCount = static_cast<int>(channels.size());

    const int inputCount = static_cast<int>(channels.size());
    const int busCount = static_cast<int>(buses.size());
    const int outputCount = static_cast<int>(outputs.size());
    header.setStatus(juce::String(inputCount) + (inputCount == 1 ? " input" : " inputs")
                     + "  -  " + juce::String(busCount)
                     + (busCount == 1 ? " bus" : " buses")
                     + "  -  " + juce::String(outputCount)
                     + (outputCount == 1 ? " output" : " outputs"));

    const auto addColumnHeader = [this](const juce::String& text) {
        auto* label = columnHeaders.add(new juce::Label());
        label->setText(text, juce::dontSendNotification);
        label->setJustificationType(juce::Justification::centred);
        label->setFont(theme::ui::itemFont());
        label->setColour(juce::Label::textColourId, theme::text());
        label->setColour(juce::Label::backgroundColourId, theme::panel());
        label->setColour(juce::Label::outlineColourId, theme::border());
        gridContent.addAndMakeVisible(*label);
    };
    for (const auto& bus : buses) addColumnHeader(bus->name);
    for (const auto& output : outputs) addColumnHeader(output->name);

    const auto addRowLabel = [this](const juce::String& text) {
        auto* label = rowLabels.add(new juce::Label());
        label->setText(text, juce::dontSendNotification);
        label->setJustificationType(juce::Justification::centredLeft);
        label->setFont(theme::ui::bodyBoldFont());
        label->setColour(juce::Label::textColourId, theme::text());
        label->setColour(juce::Label::backgroundColourId, theme::panel());
        gridContent.addAndMakeVisible(*label);
    };
    for (const auto& channel : channels) addRowLabel(channel->name);
    for (const auto& bus : buses) addRowLabel(bus->name);

    // One L/R pair per cell; the two bars are otherwise indistinguishable.
    const auto addSideLabel = [this](const juce::String& text) {
        auto* label = sendSideLabels.add(new juce::Label());
        label->setText(text, juce::dontSendNotification);
        label->setJustificationType(juce::Justification::centred);
        label->setFont(theme::ui::sectionFont());
        label->setColour(juce::Label::textColourId, theme::dim());
        label->setInterceptsMouseClicks(false, false);
        gridContent.addAndMakeVisible(*label);
        return label;
    };

    const auto addCell = [&](int column, int row, const juce::String& name,
                             float gainL, float gainR,
                             std::function<void(float)> setBoth,
                             std::function<void(float)> setL,
                             std::function<void(float)> setR) {
        Cell cell;
        cell.column = column;
        cell.row = row;
        cell.sideL = addSideLabel("L");
        cell.sideR = addSideLabel("R");
        const auto makeSlider = [&](bool rightSide) {
            auto* slider = sendSliders.add(new ValueEditSlider());
            slider->setName(name);
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
            slider->setValue(rightSide ? gainR : gainL, juce::dontSendNotification);
            slider->onValueChange = [this, slider, setBoth, setL, setR, rightSide] {
                const float v = static_cast<float>(slider->getValue());
                // Stereo link: dragging one side moves both, unless Alt
                // is held for an independent L/R trim.
                if (!juce::ModifierKeys::currentModifiers.isAltDown()) {
                    setBoth(v);
                    for (auto* other : sendSliders) {
                        if (other != nullptr && other != slider
                            && other->getName() == slider->getName())
                            other->setValue(v, juce::dontSendNotification);
                    }
                } else if (rightSide) {
                    setR(v);
                } else {
                    setL(v);
                }
            };
            gridContent.addAndMakeVisible(*slider);
            return slider;
        };
        cell.sliderL = makeSlider(false);
        cell.sliderR = makeSlider(true);
        cells.push_back(cell);
    };

    // Channel rows feed buses and outputs.
    for (int row = 0; row < channelRowCount; ++row) {
        const auto& channel = channels[static_cast<std::size_t>(row)];
        for (int column = 0; column < busColumnCount; ++column) {
            const auto& bus = buses[static_cast<std::size_t>(column)];
            const auto channelId = channel->id;
            const auto busId = bus->id;
            const juce::String name = "Send " + channel->name + " > " + bus->name;
            addCell(column, row, name,
                    busSendGain(channelId, busId, false), busSendGain(channelId, busId, true),
                    [this, channelId, busId](float v) { audioEngine.setBusSend(channelId, busId, v, v); },
                    [this, channelId, busId](float v) {
                        audioEngine.setBusSend(channelId, busId, v, busSendGain(channelId, busId, true));
                    },
                    [this, channelId, busId](float v) {
                        audioEngine.setBusSend(channelId, busId, busSendGain(channelId, busId, false), v);
                    });
        }
        for (int outputIndex = 0; outputIndex < outputCount; ++outputIndex) {
            const auto& output = outputs[static_cast<std::size_t>(outputIndex)];
            const int column = busColumnCount + outputIndex;
            const auto channelId = channel->id;
            const auto outputId = output->id;
            const juce::String name = "Send " + channel->name + " > " + output->name;
            addCell(column, row, name,
                    audioEngine.getSendGainL(channelId, outputId),
                    audioEngine.getSendGainR(channelId, outputId),
                    [this, channelId, outputId](float v) { audioEngine.setSendGain(channelId, outputId, v); },
                    [this, channelId, outputId](float v) { audioEngine.setSendGainL(channelId, outputId, v); },
                    [this, channelId, outputId](float v) { audioEngine.setSendGainR(channelId, outputId, v); });
        }
    }

    // Bus rows feed outputs only.
    for (int busIndex = 0; busIndex < busCount; ++busIndex) {
        const auto& bus = buses[static_cast<std::size_t>(busIndex)];
        const int row = channelRowCount + busIndex;
        for (int outputIndex = 0; outputIndex < outputCount; ++outputIndex) {
            const auto& output = outputs[static_cast<std::size_t>(outputIndex)];
            const int column = busColumnCount + outputIndex;
            const auto busId = bus->id;
            const auto outputId = output->id;
            const juce::String name = "Send " + bus->name + " > " + output->name;
            addCell(column, row, name,
                    busRouteGain(busId, outputId, false), busRouteGain(busId, outputId, true),
                    [this, busId, outputId](float v) { audioEngine.setBusRoute(busId, outputId, v, v); },
                    [this, busId, outputId](float v) {
                        audioEngine.setBusRoute(busId, outputId, v, busRouteGain(busId, outputId, true));
                    },
                    [this, busId, outputId](float v) {
                        audioEngine.setBusRoute(busId, outputId, busRouteGain(busId, outputId, false), v);
                    });
        }
    }

    resized();
}

float RoutingMatrixComponent::busSendGain(std::uint32_t channelId, std::uint32_t busId,
                                          bool right) const {
    for (const auto& send : audioEngine.getBusSends()) {
        if (send->channelId == channelId && send->busId == busId)
            return (right ? send->sendGainDbR : send->sendGainDbL).get();
    }
    return AudioEngine::kSendOffDb;
}

float RoutingMatrixComponent::busRouteGain(std::uint32_t busId, std::uint32_t outputId,
                                           bool right) const {
    for (const auto& route : audioEngine.getBusRoutes()) {
        if (route->busId == busId && route->outputId == outputId)
            return (right ? route->sendGainDbR : route->sendGainDbL).get();
    }
    return AudioEngine::kSendOffDb;
}
