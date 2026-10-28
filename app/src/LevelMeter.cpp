#include "LevelMeter.h"

LevelMeter::LevelMeter(juce::Atomic<float>& peakSource)
    : source(peakSource) {
    startTimerHz(30);
}

void LevelMeter::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff202020));
    auto bounds = getLocalBounds().reduced(1);
    const int meterHeight = juce::roundToInt(static_cast<float>(bounds.getHeight()) * displayedPeak);
    g.setColour(displayedPeak >= 1.0f ? juce::Colour(0xffe05252) : juce::Colour(0xff56b870));
    g.fillRect(bounds.removeFromBottom(meterHeight));
    g.setColour(juce::Colours::grey);
    g.drawRect(getLocalBounds());
}

void LevelMeter::timerCallback() {
    const auto newPeak = juce::jlimit(0.0f, 1.0f, source.exchange(0.0f));
    displayedPeak = juce::jmax(newPeak, displayedPeak * 0.85f);
    repaint();
}
