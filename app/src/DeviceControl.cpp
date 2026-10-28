#include "DeviceControl.h"

DeviceControl::DeviceControl() {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void DeviceControl::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    juce::Colour face = theme::control();
    if (hovered) face = theme::control().brighter(0.08f);
    if (pressed) face = theme::control().darker(0.08f);

    g.setColour(face);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(theme::border());
    g.drawRoundedRectangle(r, 4.0f, 1.0f);

    if (hasKeyboardFocus(true)) {
        g.setColour(theme::cyan());
        g.drawRoundedRectangle(r.reduced(1.0f), 4.0f, 1.0f);
    }

    auto area = getLocalBounds().reduced(8, 4);
    // right side: dot + chevron
    auto right = area.removeFromRight(26);
    // chevron
    {
        juce::Path p;
        auto cr = right.removeFromRight(10).toFloat().reduced(2.0f);
        p.startNewSubPath(cr.getX(), cr.getY() + 2);
        p.lineTo(cr.getCentreX(), cr.getBottom() - 2);
        p.lineTo(cr.getRight(), cr.getY() + 2);
        g.setColour(theme::dim());
        g.strokePath(p, juce::PathStrokeType(1.4f));
    }
    // dot
    {
        juce::Colour dot = theme::dim(); // none
        if (error.isNotEmpty()) dot = theme::red();
        else if (deviceName.isNotEmpty() && !deviceName.startsWith("None")) dot = theme::green();
        auto dr = right.removeFromLeft(8).toFloat();
        auto dotR = juce::Rectangle<float>(dr.getX() + (dr.getWidth() - 6.0f) / 2.0f,
                                           dr.getCentreY() - 3.0f, 6.0f, 6.0f);
        g.setColour(dot);
        g.fillEllipse(dotR);
    }

    // left text
    auto textArea = area;
    g.setColour(theme::text());
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    g.drawText(title, textArea.removeFromTop(textArea.getHeight() / 2), juce::Justification::centredLeft, true);

    juce::String sub = deviceName.isNotEmpty() ? deviceName : juce::String("Click to choose device");
    if (deviceName.isEmpty()) sub = "Click to choose device";
    g.setColour(theme::dim());
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText(sub, textArea, juce::Justification::centredLeft, true);
}

void DeviceControl::mouseDown(const juce::MouseEvent&) {
    pressed = true;
    repaint();
    if (onClick) onClick();
}

bool DeviceControl::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey) {
        if (onClick) onClick();
        return true;
    }
    return false;
}
