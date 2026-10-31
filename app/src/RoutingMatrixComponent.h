#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"

class RoutingMatrixComponent final : public juce::Component {
public:
    explicit RoutingMatrixComponent(AudioEngine& engine);

    void resized() override;
    void paint(juce::Graphics& g) override;
    void rebuild();

private:
    AudioEngine& audioEngine;
    juce::Viewport gridViewport;
    juce::Component gridContent;
    juce::OwnedArray<juce::Label> outputHeaders;
    juce::OwnedArray<juce::Label> channelLabels;
    juce::OwnedArray<juce::Slider> sendSliders;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RoutingMatrixComponent)
};
