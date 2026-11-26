#pragma once

#include <JuceHeader.h>

class ConsoleLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    ConsoleLookAndFeel();

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;

    // Every slider value box is a numeric readout, so it is created in the
    // monospace face once here instead of being restyled slider by slider.
    // A dragged fader or a routing send then never reflows its own text.
    juce::Label* createSliderTextBox(juce::Slider&) override;

    // One button face for the whole app: same radius, same hover/press delta,
    // the toggle colour for an active state and a cyan focus ring so keyboard
    // focus is visible everywhere without per-button styling.
    void drawButtonBackground(juce::Graphics&, juce::Button&,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;
};
