#pragma once

#include <JuceHeader.h>

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
    AudioEngine& audioEngine;
    PageHeader header{ "Routing" };
    juce::Viewport gridViewport;
    juce::Component gridContent;
    juce::OwnedArray<juce::Label> outputHeaders;
    juce::OwnedArray<juce::Label> channelLabels;
    juce::OwnedArray<juce::Label> sendSideLabels;
    juce::OwnedArray<ValueEditSlider> sendSliders;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RoutingMatrixComponent)
};
