#pragma once

#include <JuceHeader.h>

// Stereo peak meter for a strip's fader column: left and right are drawn side
// by side on the same -60..+12 dB window the fader uses, so a mark in the meter
// lines up with the same dB on the fader beside it.
class LevelMeter final : public juce::Component,
                         private juce::Timer {
public:
    LevelMeter(juce::Atomic<float>& leftPeakSource, juce::Atomic<float>& rightPeakSource);

    // The vertical extent of a fader's track, in the fader's parent
    // coordinates. A slider's own bounds also hold its value box, and the look
    // and feel insets the track further, so a meter laid out on the whole
    // component would read taller than the fader it sits next to.
    static juce::Rectangle<int> faderTrackBounds(juce::Slider& fader);

    void paint(juce::Graphics& g) override;

    static constexpr float kMinDb = -60.0f;
    static constexpr float kMaxDb = 12.0f;

private:
    void timerCallback() override;

    juce::Atomic<float>& leftSource;
    juce::Atomic<float>& rightSource;
    float displayedLeft = 0.0f;
    float displayedRight = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeter)
};
