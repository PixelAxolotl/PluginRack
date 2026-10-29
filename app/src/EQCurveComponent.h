#pragma once

#include <JuceHeader.h>

// Shell placeholder for the Task 5 EQ curve editor. Paints a dim placeholder
// label; Task 5 replaces the paint with the real six-band curve + handles.
class EQCurveComponent final : public juce::Component {
public:
    EQCurveComponent();

    void paint(juce::Graphics& g) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EQCurveComponent)
};
