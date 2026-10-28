#include "ConsoleLookAndFeel.h"

#include "Theme.h"

ConsoleLookAndFeel::ConsoleLookAndFeel() {
    using namespace theme;
    setColour(juce::PopupMenu::backgroundColourId, panel());
    setColour(juce::PopupMenu::textColourId, text());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, border());
    setColour(juce::PopupMenu::highlightedTextColourId, bg());
    setColour(juce::PopupMenu::headerTextColourId, dim());

    setColour(juce::AlertWindow::backgroundColourId, panel());
    setColour(juce::AlertWindow::textColourId, text());
    setColour(juce::AlertWindow::outlineColourId, border());

    setColour(juce::TooltipWindow::backgroundColourId, panel());
    setColour(juce::TooltipWindow::textColourId, text());
    setColour(juce::TooltipWindow::outlineColourId, border());
}
