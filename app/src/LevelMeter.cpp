#include "LevelMeter.h"

#include <cmath>

#include "Theme.h"

LevelMeter::LevelMeter(juce::Atomic<float>& peakSource)
    : source(peakSource) {
    startTimerHz(30);
}

void LevelMeter::paint(juce::Graphics& g) {
    g.fillAll(theme::bg().darker(0.2f));
    auto bounds = getLocalBounds().reduced(3, 2);
    const int totalH = bounds.getHeight();
    // Same -60..+12 dB window the fader is drawn on, so the meter next to a
    // fader lines up with it (and with the dynamics panel's dB scales).
    const float db = 20.0f * std::log10(juce::jmax(displayedPeak, 1.0e-6f));
    const float proportion = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 72.0f);
    const int meterHeight = juce::roundToInt(static_cast<float>(totalH) * proportion);

    // Track and scale copied from the dynamics panel's level meters: the same
    // green-at-the-bottom to amber-at-the-top ramp over the same panel well, so
    // a level reads the same in the mixer as it does on the compressor page.
    g.setColour(theme::panel());
    g.fillRoundedRectangle(bounds.toFloat(), 2.0f);
    g.setColour(theme::line().withAlpha(0.45f));
    g.drawRoundedRectangle(bounds.toFloat(), 2.0f, 1.0f);
    if (meterHeight > 0) {
        juce::ColourGradient grad(theme::green(), (float)bounds.getX(), (float)bounds.getBottom(),
                                 theme::amber(), (float)bounds.getX(), (float)bounds.getY(), false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle((float)bounds.getX(), (float)(bounds.getBottom() - meterHeight),
                               (float)bounds.getWidth(), (float)meterHeight, 2.0f);
    }

    g.setColour(theme::line().withAlpha(0.65f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 2.0f, 1.0f);
}

void LevelMeter::timerCallback() {
    const auto newPeak = juce::jlimit(0.0f, 1.0f, source.exchange(0.0f));
    displayedPeak = juce::jmax(newPeak, displayedPeak * 0.85f);
    repaint();
}