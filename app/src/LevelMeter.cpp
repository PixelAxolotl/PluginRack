#include "LevelMeter.h"

#include <cmath>

#include "Theme.h"

namespace {

constexpr float kDbSpan = LevelMeter::kMaxDb - LevelMeter::kMinDb;

// dB marks on the meter: the fader's own endpoints plus the steps a stereo
// meter is read at.
constexpr int kMarks[] = {12, 6, 0, -6, -12, -24, -36, -48, -60};

float proportionOf(float peak) {
    const float db = 20.0f * std::log10(juce::jmax(peak, 1.0e-6f));
    return juce::jlimit(0.0f, 1.0f, (db - LevelMeter::kMinDb) / kDbSpan);
}

} // namespace

juce::Rectangle<int> LevelMeter::faderTrackBounds(juce::Slider& fader) {
    const auto slider = fader.getLookAndFeel().getSliderLayout(fader).sliderBounds;
    const int inset = juce::jmin(theme::metrics::kFaderTrackInset, slider.getHeight() / 2);
    return slider.reduced(0, inset).translated(fader.getX(), fader.getY());
}

LevelMeter::LevelMeter(juce::Atomic<float>& leftPeakSource, juce::Atomic<float>& rightPeakSource)
    : leftSource(leftPeakSource), rightSource(rightPeakSource) {
    startTimerHz(30);
}

void LevelMeter::setStereo(bool shouldBeStereo) {
    if (stereo == shouldBeStereo) return;
    stereo = shouldBeStereo;
    repaint();
}

void LevelMeter::paint(juce::Graphics& g) {
    g.fillAll(theme::bg().darker(0.2f));

    const auto well = getLocalBounds().reduced(1);
    if (well.isEmpty()) return;
    g.setColour(theme::panel());
    g.fillRoundedRectangle(well.toFloat(), 2.0f);

    const auto bars = well.reduced(1);
    if (bars.isEmpty()) return;
    constexpr int gap = 2;
    const int barWidth = stereo ? juce::jmax(1, (bars.getWidth() - gap) / 2)
                                : bars.getWidth();
    const juce::Rectangle<int> leftBar{bars.getX(), bars.getY(), barWidth, bars.getHeight()};
    const juce::Rectangle<int> rightBar{bars.getRight() - barWidth, bars.getY(), barWidth,
                                        bars.getHeight()};

    // Each side gets its own dark track in stereo; mono uses the full meter
    // width and renders only the left/source bar.
    g.setColour(theme::bg());
    g.fillRoundedRectangle(leftBar.toFloat(), 1.5f);
    if (stereo) g.fillRoundedRectangle(rightBar.toFloat(), 1.5f);

    const auto markY = [&bars](int db) {
        const float proportion = (static_cast<float>(db) - kMinDb) / kDbSpan;
        const int top = bars.getY();
        const int bottom = bars.getBottom() - 1;
        return juce::jlimit(top, bottom,
                            juce::roundToInt(static_cast<float>(bottom)
                                             - static_cast<float>(bottom - top) * proportion));
    };

    // Ticks first on the empty track, then again over the bars: one pass would
    // either vanish against the panel or vanish against a lit bar.
    for (const int db : kMarks) {
        g.setColour(theme::line().withAlpha(db == 0 ? 0.55f : 0.3f));
        g.drawHorizontalLine(markY(db), (float)bars.getX(), (float)bars.getRight());
    }

    const auto drawBar = [&g](juce::Rectangle<int> bar, float proportion) {
        const int filled = juce::roundToInt(static_cast<float>(bar.getHeight()) * proportion);
        if (filled <= 0) return;
        juce::ColourGradient grad(theme::green(), (float)bar.getX(), (float)bar.getBottom(),
                                 theme::amber(), (float)bar.getX(), (float)bar.getY(), false);
        g.setGradientFill(grad);
        g.fillRect(bar.getX(), bar.getBottom() - filled, bar.getWidth(), filled);
    };
    drawBar(leftBar, proportionOf(displayedLeft));
    drawBar(rightBar, proportionOf(displayedRight));

    for (const int db : kMarks) {
        g.setColour(theme::bg().withAlpha(db == 0 ? 0.6f : 0.38f));
        g.drawHorizontalLine(markY(db), (float)bars.getX(), (float)bars.getRight());
    }

    // Each bar is named at its foot. The chip keeps the letter readable
    // whether or not the bar underneath it is lit.
    const int labelHeight = juce::jmin(10, bars.getHeight());
    if (labelHeight >= 8) {
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
            const int sideCount = stereo ? 2 : 0;
            for (int side = 0; side < sideCount; ++side) {
                const auto bar = side == 0 ? leftBar : rightBar;
                const auto chip = bar.withY(bar.getBottom() - labelHeight).withHeight(labelHeight);
                g.setColour(theme::bg().withAlpha(0.85f));
                g.fillRect(chip);
                g.setColour(theme::text());
                g.drawText(side == 1 ? "R" : "L", chip, juce::Justification::centred, false);
            }
    }

    g.setColour(theme::line().withAlpha(0.65f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 2.0f, 1.0f);
}

void LevelMeter::timerCallback() {
    const auto newLeft = juce::jlimit(0.0f, 1.0f, leftSource.exchange(0.0f));
    const auto newRight = juce::jlimit(0.0f, 1.0f, rightSource.exchange(0.0f));
    displayedLeft = juce::jmax(newLeft, displayedLeft * 0.85f);
    displayedRight = juce::jmax(newRight, displayedRight * 0.85f);
    repaint();
}