#pragma once

#include <JuceHeader.h>

class LevelMeter final : public juce::Component,
                         private juce::Timer {
public:
    explicit LevelMeter(juce::Atomic<float>& peakSource);

    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;

    juce::Atomic<float>& source;
    float displayedPeak = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeter)
};
