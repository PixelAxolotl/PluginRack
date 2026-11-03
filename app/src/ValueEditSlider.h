#pragma once

#include <JuceHeader.h>

// Slider whose value can also be typed in: a right-click drops the caret into
// the value box below the knob with the whole number selected, so the next
// keystroke replaces it and Enter (or clicking away) applies it through the
// normal notification path - the slider's onValueChange handler (and the engine)
// sees the edit exactly as if the knob had been dragged.
//
// The right-click is swallowed here on purpose: juce::Slider would otherwise
// treat it as the start of a drag, and a linear slider jumps its value to the
// clicked position the moment the press lands.
class ValueEditSlider final : public juce::Slider {
public:
    ValueEditSlider() {
        // Deep listener on itself: that is how a right-click on the slider's
        // value text box (a child Label) reaches this component as well.
        addMouseListener(this, true);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (e.eventComponent != this) {
            // Nested child (the value box). The base slider must never see
            // these events or a label click would start a drag.
            if (e.mods.isPopupMenu()) pendingEdit = true;
            return;
        }
        if (e.mods.isPopupMenu()) {
            pendingEdit = true;
            return;
        }
        juce::Slider::mouseDown(e);
    }

    // The edit session starts on release, not on press: starting it while the
    // press is still in flight let the matching mouse-up dismiss the freshly
    // opened editor.
    void mouseUp(const juce::MouseEvent& e) override {
        if (pendingEdit) {
            pendingEdit = false;
            if (isEnabled()) showValueEditor();
            return;
        }
        if (e.eventComponent != this) return;
        juce::Slider::mouseUp(e);
    }

    // The deep listener above also feeds the slider its own children's events;
    // keep those away from the base class so nothing but the right-click above
    // changes how a touch on the value box behaves.
    void mouseDrag(const juce::MouseEvent& e) override {
        if (e.eventComponent != this) return;
        juce::Slider::mouseDrag(e);
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        if (e.eventComponent != this) return;
        juce::Slider::mouseWheelMove(e, wheel);
    }

private:
    void showValueEditor();
    juce::Label* findValueBox() const;
    bool pendingEdit = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ValueEditSlider)
};
