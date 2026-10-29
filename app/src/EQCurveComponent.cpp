#include "EQCurveComponent.h"

#include "Theme.h"

EQCurveComponent::EQCurveComponent() = default;

void EQCurveComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::strip());
    g.setColour(theme::dim());
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText("EQ curve - full editor in Task 5", getLocalBounds(),
               juce::Justification::centred, true);
}
