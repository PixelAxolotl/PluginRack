#pragma once
#include <JuceHeader.h>
#include <functional>
class GraphComponent final : public juce::Component {
public:
    GraphComponent() = default;
    std::function<void(juce::Graphics&)> painter;
    void paint(juce::Graphics& g) override { if (painting) return; painting = true; if (painter) painter(g); painting = false; }
    bool painting = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphComponent)
};
