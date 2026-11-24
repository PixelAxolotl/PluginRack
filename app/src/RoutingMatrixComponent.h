#pragma once

#include <JuceHeader.h>

#include <vector>

#include "AudioEngine.h"
#include "PageHeader.h"
#include "Theme.h"
#include "ValueEditSlider.h"

class RoutingMatrixComponent final : public juce::Component {
public:
    explicit RoutingMatrixComponent(AudioEngine& engine);

    void resized() override;
    void paint(juce::Graphics& g) override;
    void rebuild();

private:
    // One L/R pair inside the grid. Bus rows have no bus columns, so cells
    // are described rather than indexed by a dense channel x column formula.
    struct Cell {
        int column = 0;
        int row = 0;
        juce::Label* sideL = nullptr;
        juce::Label* sideR = nullptr;
        ValueEditSlider* sliderL = nullptr;
        ValueEditSlider* sliderR = nullptr;
    };

    AudioEngine& audioEngine;
    PageHeader header{ "Routing" };
    juce::Viewport gridViewport;
    juce::Component gridContent;
    juce::OwnedArray<juce::Label> columnHeaders;
    juce::Label busBand{"", "BUSES"};
    juce::Label outputBand{"", "OUTPUTS"};
    juce::OwnedArray<juce::Label> rowLabels;
    juce::OwnedArray<juce::Label> sendSideLabels;
    juce::OwnedArray<ValueEditSlider> sendSliders;
    std::vector<Cell> cells;
    int busColumnCount = 0;
    int channelRowCount = 0;

    float busSendGain(std::uint32_t channelId, std::uint32_t busId, bool right) const;
    float busRouteGain(std::uint32_t busId, std::uint32_t outputId, bool right) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RoutingMatrixComponent)
};
