#pragma once

#include <JuceHeader.h>

#include <functional>

#include "Theme.h"

/** Small square console button used inside insert slots. */
class MiniButton final : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit MiniButton(juce::String text) : label(std::move(text)) {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus(true);
    }

    void setToggleState(bool state) {
        if (on == state) return;
        on = state;
        repaint();
    }
    bool getToggleState() const { return on; }
    void setLabel(juce::String newLabel) {
        if (label == newLabel) return;
        label = std::move(newLabel);
        repaint();
    }
    void setClickAction(std::function<void()> action) { clickAction = std::move(action); }
    // Strip convention (EqButton): the left button toggles, the right button
    // opens the effect's editor.
    std::function<void()> onRightClick;
    void setEnabledState(bool state) { active = state; repaint(); }
    void setFontSize(float size) { fontSize = size; repaint(); }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        juce::Colour face = on ? theme::violet() : theme::control();
        if (!active) face = face.withAlpha(0.35f);
        else if (hovered) face = face.brighter(0.1f);
        if (pressed && active) face = face.darker(0.12f);

        g.setColour(face);
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(active ? theme::border() : theme::border().withAlpha(0.4f));
        g.drawRoundedRectangle(r, 3.0f, 1.0f);

        if (hasKeyboardFocus(true)) {
            g.setColour(theme::cyan());
            g.drawRoundedRectangle(r.reduced(1.0f), 3.0f, 1.0f);
        }

        juce::Colour textColour = theme::text();
        if (on) textColour = juce::Colour(0xff1b2229);
        if (!active) textColour = theme::dim().withAlpha(0.5f);
        g.setColour(textColour);
        g.setFont(juce::Font(juce::FontOptions(fontSize, juce::Font::bold)));
        g.drawText(label, getLocalBounds(), juce::Justification::centred);
    }

    void mouseEnter(const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered = false; pressed = false; repaint(); }
    void mouseUp(const juce::MouseEvent&) override { pressed = false; repaint(); }

    void mouseDown(const juce::MouseEvent& e) override {
        if (!active) return;
        if (e.mods.isPopupMenu()) {
            if (onRightClick) onRightClick();
            return;
        }
        pressed = true;
        repaint();
        if (clickAction) clickAction();
    }

private:
    juce::String label;
    std::function<void()> clickAction;
    float fontSize = 10.0f;
    bool on = false;
    bool active = true;
    bool hovered = false;
    bool pressed = false;
};
