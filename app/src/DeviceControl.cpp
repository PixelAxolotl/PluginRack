#include "DeviceControl.h"

#include "DeviceBackend.h"

namespace {
const juce::String kSystemDefault = "System Default";

juce::String compactDeviceName(const juce::String& name) {
    if (name == kSystemDefault) return "Default";
    if (name.startsWith("None")) return "None";
    auto trimmed = name;
    const int paren = trimmed.indexOfChar('(');
    if (paren > 1) trimmed = trimmed.substring(0, paren).trim();
    return trimmed;
}
}

DeviceControl::DeviceControl() {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void DeviceControl::updateTooltip() {
    juce::String text = title;
    if (deviceName.isNotEmpty()) text += "\n" + deviceName;
    if (error.isNotEmpty()) text += "\n" + error;
    text += "\nDouble-click to rename";
    setTooltip(text);
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

    const bool narrow = getWidth() < 110;
    auto area = getLocalBounds().reduced(narrow ? 5 : 8, 4);
    // right side: dot + chevron
    auto right = area.removeFromRight(26);
    // chevron
    {
        juce::Path p;
        auto chevronArea = right.removeFromRight(10).toFloat();
        auto cr = juce::Rectangle<float>(chevronArea.getCentreX() - 3.5f,
                                         chevronArea.getCentreY() - 2.0f, 7.0f, 4.0f);
        p.startNewSubPath(cr.getX(), cr.getY());
        p.lineTo(cr.getCentreX(), cr.getBottom());
        p.lineTo(cr.getRight(), cr.getY());
        g.setColour(theme::dim());
        g.strokePath(p, juce::PathStrokeType(1.2f));
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
    g.setFont(theme::ui::itemFont());
    g.drawText(title, textArea.removeFromTop(textArea.getHeight() / 2), juce::Justification::centredLeft, true);

    juce::String sub;
    juce::Colour subColour = theme::dim();
    if (error.isNotEmpty()) {
        sub = error;
        subColour = theme::red();
    } else if (deviceName.isEmpty()) {
        sub = narrow ? "assign" : "Choose device";
    } else {
        sub = narrow ? compactDeviceName(deviceName) : deviceName;
    }
    g.setColour(subColour);
    g.setFont(juce::Font(juce::FontOptions(narrow ? 10.0f : 11.0f)));

    // Backend tag on the right of the device line: "ASIO" or "WASAPI" answers
    // the one question the device name cannot.
    const auto tag = deviceBackendTag(backendType);
    if (tag.isNotEmpty() && error.isEmpty()) {
        const auto tagFont = juce::Font(juce::FontOptions(9.0f, juce::Font::bold));
        const int tagWidth = juce::GlyphArrangement::getStringWidthInt(tagFont, tag) + 10;
        auto tagArea = textArea.removeFromRight(juce::jmin(tagWidth, textArea.getWidth() / 2));
        g.setColour(deviceBackendIsAsio(backendType) ? theme::green() : theme::dim());
        g.setFont(tagFont);
        g.drawText(tag, tagArea, juce::Justification::centredRight, false);
        g.setFont(juce::Font(juce::FontOptions(narrow ? 10.0f : 11.0f)));
    }

    g.drawText(sub, textArea, juce::Justification::centredLeft, true);
}

void DeviceControl::mouseDown(const juce::MouseEvent& e) {
    pressed = true;
    repaint();
    if (e.mods.isRightButtonDown()) {
        if (onRename) onRename();
        return;
    }
    if (onClick) onClick();
}

void DeviceControl::mouseDoubleClick(const juce::MouseEvent&) {
    // One action per double-click: the tooltip promises rename, and a strip
    // that also navigates on the same gesture would do both at once.
    if (onRename) onRename();
    else if (onDoubleClick) onDoubleClick();
}

bool DeviceControl::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey) {
        if (onClick) onClick();
        return true;
    }
    return false;
}
