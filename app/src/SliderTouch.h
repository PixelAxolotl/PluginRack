#pragma once

#include <JuceHeader.h>

#include <functional>

// Remembers the dial the user touched last (the rack's touch-and-slide strip
// drives it) and gives sliders that carry no value box of their own - foreign
// JUCE dials included - the same right-click value entry the app's own dials
// have.
//
// A "dial" is anything the strip can move: one of the app's sliders, or a VST
// parameter picked up when the user turns a knob inside the plugin's own
// window. Everything goes through the Dial callbacks, so a dial that has since
// been destroyed simply stops responding instead of being dereferenced.
namespace slider_touch {

struct Dial {
    juce::String name;
    std::function<double()> getProportion;      // 0 = minimum, 1 = maximum
    std::function<void(double)> setProportion;  // input clamped to 0..1
    std::function<juce::String()> getText;      // display text, like the dial shows
    // Where the dial's own default sits (0..1), or a negative value when it has
    // none. The strip's double-click reset and snap-to-default use it.
    double defaultProportion = -1.0;
};

// Marks one of the app's sliders as the dial the strip should drive.
void record(juce::Slider& slider);
// Marks an arbitrary dial (a plugin parameter) as the one to drive.
void recordDial(Dial dial);
// Drops the current dial, e.g. when the plugin it belongs to is going away.
void clear();
// The dial to drive, or nullptr when nothing has been touched yet.
const Dial* current();

// Single observer for "the dial changed"; pass {} to clear.
void setObserver(std::function<void()> observer);

// True while the touch strip is being dragged. Plugin-side gestures are
// ignored then, so a plugin that moves several parameters at once (or starts a
// gesture on a linked one) cannot steal the strip mid-slide.
void beginDriving();
void endDriving();
bool isDriving();

// Installs the application-wide right-click handler (idempotent). Call once
// after the message loop exists.
void installRightClickEditor();

} // namespace slider_touch
