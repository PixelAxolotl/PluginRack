#include "LevelMeter.h"

#include "Theme.h"

LevelMeter::LevelMeter(juce::Atomic<float>& peakSource)
    : source(peakSource) {
    startTimerHz(30);
}

void LevelMeter::paint(juce::Graphics& g) {
    g.fillAll(theme::strip());
    auto bounds = getLocalBounds().reduced(1);
    const int totalH = bounds.getHeight();
    const int meterHeight = juce::roundToInt(static_cast<float>(totalH) * displayedPeak);

    juce::ColourGradient grad(theme::green(), (float)bounds.getX(), (float)bounds.getBottom(),
                             theme::red(), (float)bounds.getX(), (float)bounds.getY(), false);
    grad.addColour(0.7, theme::green());
    grad.addColour(0.85, theme::amber());
    g.setGradientFill(grad);
    g.fillRect(bounds.getX(), bounds.getBottom() - meterHeight, bounds.getWidth(), meterHeight);

    g.setColour(theme::border());
    g.drawRect(getLocalBounds());
}

void LevelMeter::timerCallback() {
    const auto newPeak = juce::jlimit(0.0f, 1.0f, source.exchange(0.0f));
    displayedPeak = juce::jmax(newPeak, displayedPeak * 0.85f);
    repaint();
}