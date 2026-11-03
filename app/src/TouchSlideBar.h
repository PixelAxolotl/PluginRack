#pragma once

#include <JuceHeader.h>

// Touch-friendly "TOUCH AND SLIDE" strip for the rack: it drives whichever dial
// was touched last (see slider_touch::current()). That dial can be one of the
// app's sliders or a knob the user turned inside a VST's own window.
//
// The gesture is relative on purpose: pressing the strip never jumps the dial,
// the slide then sweeps the dial's full range over the strip's height, and the
// value is applied in proportion space so dials with a skew (frequency, delay,
// plugin units) stay usable. Near the dial's default the slide snaps onto it,
// the default is marked by a thicker tick, and a double-click/tap resets the
// dial to that default. The strip keeps repainting so it tracks edits made on
// the dial itself.
class TouchSlideBar final : public juce::Component, private juce::Timer {
public:
    TouchSlideBar();
    ~TouchSlideBar() override;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    void timerCallback() override;
    juce::Rectangle<int> trackBounds() const;
    void applyProportion(double proportion);

    double dragStartProportion = 0.0;
    int dragStartY = 0;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TouchSlideBar)
};
