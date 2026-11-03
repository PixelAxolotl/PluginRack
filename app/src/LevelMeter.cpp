#include "LevelMeter.h"

#include <cmath>

#include "Theme.h"

namespace {

constexpr float kDbSpan = LevelMeter::kMaxDb - LevelMeter::kMinDb;

float proportionOf(float level) {
    if (level <= 0.0f) return 0.0f;
    const float db = 20.0f * std::log10(level);
    return juce::jlimit(0.0f, 1.0f, (db - LevelMeter::kMinDb) / kDbSpan);
}

juce::String dbText(float level) {
    if (level <= 0.0f) return "-Inf";
    const float db = 20.0f * std::log10(level);
    return db <= LevelMeter::kMinDb ? juce::String("-Inf") : juce::String(db, 1);
}

juce::String intervalText(int ms) {
    return ms >= 1000 ? juce::String(ms / 1000) + " s" : juce::String(ms) + " ms";
}

int yForLevel(juce::Rectangle<int> bar, float proportion) {
    return juce::jlimit(bar.getY(), bar.getBottom(),
                        juce::roundToInt(static_cast<float>(bar.getBottom())
                                         - static_cast<float>(bar.getHeight()) * proportion));
}

// Fast attack, slower fall: the RMS bar follows the ear rather than the block
// boundary, so a 32-sample block does not make it flicker.
float smoothedRms(float current, float target) {
    const float weight = target > current ? 0.5f : 0.12f;
    return current + (target - current) * weight;
}

} // namespace

juce::Rectangle<int> LevelMeter::faderValueBounds(juce::Slider& fader) {
    return fader.getLookAndFeel().getSliderLayout(fader)
        .sliderBounds.translated(fader.getX(), fader.getY());
}

LevelMeter::LevelMeter(juce::Slider& faderToFollow, juce::Atomic<float>& leftPeak,
                       juce::Atomic<float>& rightPeak, juce::Atomic<float>& leftRms,
                       juce::Atomic<float>& rightRms)
    : fader(faderToFollow),
      leftPeakSource(leftPeak),
      rightPeakSource(rightPeak),
      leftRmsSource(leftRms),
      rightRmsSource(rightRms) {
    // The strip may be built while audio is already running; start from the
    // live values instead of an empty bar.
    displayedPeakL = heldPeakL = leftPeakSource.get();
    displayedPeakR = heldPeakR = rightPeakSource.get();
    displayedRmsL = leftRmsSource.get();
    displayedRmsR = rightRmsSource.get();
    setTooltip("Levels - right-click to set the peak-hold interval");
    startTimerHz(30);
}

int LevelMeter::peakHoldMs() {
    return peakHoldMsValue.load(std::memory_order_relaxed);
}

void LevelMeter::setPeakHoldMs(int ms) {
    peakHoldMsValue.store(juce::jlimit(100, 1000, ms), std::memory_order_relaxed);
}

void LevelMeter::mouseDown(const juce::MouseEvent& event) {
    if (!event.mods.isPopupMenu()) return;

    juce::PopupMenu menu;
    menu.addSectionHeader("Peak hold");
    const int current = peakHoldMs();
    for (const int ms : kPeakHoldChoices) {
        menu.addItem(intervalText(ms), true, ms == current,
                     [ms] { LevelMeter::setPeakHoldMs(ms); });
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
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

    auto content = well.reduced(1);
    if (content.getWidth() < 12 || content.getHeight() < 12) {
        g.setColour(theme::line().withAlpha(0.65f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 2.0f, 1.0f);
        return;
    }

    auto scaleArea = content.removeFromRight(juce::jmin(16, content.getWidth() / 2));
    content.removeFromRight(2);

    // Bars live on the fader's travel so a level lines up with the same dB on
    // the fader beside it. Everything above that travel holds the readouts.
    int barTop = content.getY();
    int barBottom = content.getBottom();
    const auto travel = faderValueBounds(fader).translated(-getX(), -getY());
    if (!travel.isEmpty()) {
        barTop = juce::jlimit(content.getY(), content.getBottom() - 1, travel.getY());
        barBottom = juce::jlimit(barTop + 1, content.getBottom(), travel.getBottom());
    }
    const int barHeight = barBottom - barTop;

    const int readoutHeight = juce::jmin(22, barTop - well.getY() - 1);
    if (readoutHeight >= 16) {
        auto readouts = juce::Rectangle<int>(well.getX() + 2, barTop - readoutHeight - 1,
                                             well.getWidth() - 4, readoutHeight);
        auto peakRow = readouts.removeFromTop(readoutHeight / 2);
        const auto drawReadout = [&g](juce::Rectangle<int> row, const juce::String& value,
                                      const juce::String& name) {
            const auto valueFont = juce::Font(juce::FontOptions(9.0f));
            const auto nameFont = juce::Font(juce::FontOptions(7.0f));

            // Draw the glyphs at their natural size and place them by their
            // own bounds: a fixed rect either squeezes a wide value or clips
            // its last digit on this narrow meter.
            juce::GlyphArrangement nameGlyphs;
            nameGlyphs.addLineOfText(nameFont, name, 0.0f, 0.0f);
            const auto nameBounds = nameGlyphs.getBoundingBox(0, -1, true);

            juce::GlyphArrangement valueGlyphs;
            valueGlyphs.addLineOfText(valueFont, value, 0.0f, 0.0f);
            const auto valueBounds = valueGlyphs.getBoundingBox(0, -1, true);

            const int nameRight = row.getRight();
            const int valueRight = nameRight - juce::roundToInt(nameBounds.getWidth()) - 3;
            const auto place = [&row](const juce::Rectangle<float>& bounds, int right) {
                return juce::AffineTransform::translation(
                    (float)right - bounds.getRight(),
                    (float)row.getY()
                        + ((float)row.getHeight() - bounds.getHeight()) * 0.5f - bounds.getY());
            };

            g.setColour(theme::text());
            valueGlyphs.draw(g, place(valueBounds, valueRight));
            g.setColour(theme::dim());
            nameGlyphs.draw(g, place(nameBounds, nameRight));
        };
        const float loudestPeak = stereo ? juce::jmax(displayedPeakL, displayedPeakR) : displayedPeakL;
        const float loudestRms = stereo ? juce::jmax(displayedRmsL, displayedRmsR) : displayedRmsL;
        drawReadout(peakRow, dbText(loudestPeak), "Peak");
        drawReadout(readouts, dbText(loudestRms), "RMS");
    }

    constexpr int gap = 2;
    const int barWidth = stereo ? juce::jmax(1, (content.getWidth() - gap) / 2)
                                : content.getWidth();
    const juce::Rectangle<int> leftBar{content.getX(), barTop, barWidth, barHeight};
    const juce::Rectangle<int> rightBar{content.getRight() - barWidth, barTop, barWidth,
                                        barHeight};

    // Each side gets its own dark track in stereo; mono uses the full meter
    // width and renders only the left/source bar.
    g.setColour(theme::bg());
    g.fillRoundedRectangle(leftBar.toFloat(), 1.5f);
    if (stereo) g.fillRoundedRectangle(rightBar.toFloat(), 1.5f);

    const auto drawSide = [&g](juce::Rectangle<int> bar, float rmsProportion,
                               float peakProportion, float holdProportion) {
        const int rmsTop = yForLevel(bar, rmsProportion);
        // RMS can crest over a peak that is already releasing; keep the peak
        // block on top of the RMS bar instead of inverting them.
        const int peakTop = yForLevel(bar, juce::jmax(peakProportion, rmsProportion));
        if (peakTop < rmsTop) {
            g.setColour(theme::line().withAlpha(0.75f));
            g.fillRect(bar.getX(), peakTop, bar.getWidth(), rmsTop - peakTop);
        }
        if (rmsTop < bar.getBottom()) {
            juce::ColourGradient grad(theme::green(), (float)bar.getX(), (float)bar.getBottom(),
                                     theme::amber(), (float)bar.getX(), (float)bar.getY(), false);
            g.setGradientFill(grad);
            g.fillRect(bar.getX(), rmsTop, bar.getWidth(), bar.getBottom() - rmsTop);
        }
        if (holdProportion > 0.0f) {
            const int holdY = juce::jlimit(bar.getY(), bar.getBottom() - 2,
                                           yForLevel(bar, holdProportion));
            g.setColour(theme::text());
            g.fillRect(bar.getX(), holdY, bar.getWidth(), 2);
        }
    };

    drawSide(leftBar, proportionOf(displayedRmsL), proportionOf(displayedPeakL),
             proportionOf(heldPeakL));
    if (stereo)
        drawSide(rightBar, proportionOf(displayedRmsR), proportionOf(displayedPeakR),
                 proportionOf(heldPeakR));

    // dB scale, thinned so the marks never collide on a short meter. The
    // topmost mark is pushed below the bar's top edge so it cannot run into the
    // readouts above.
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    int lastLabelBottom = -10000;
    for (const int db : theme::metrics::kDbMarks) {
        const float proportion = (static_cast<float>(db) - kMinDb) / kDbSpan;
        const int y = yForLevel(leftBar, proportion);
        const int labelTop = juce::jlimit(barTop, juce::jmax(barTop, barBottom - 10), y - 5);
        if (labelTop < lastLabelBottom + 2) continue;
        g.setColour(theme::dim());
        g.drawText(juce::String(db), scaleArea.getX(), labelTop, scaleArea.getWidth(), 10,
                   juce::Justification::centredLeft, false);
        lastLabelBottom = labelTop + 10;
    }

    // Each bar is named at its foot. The chip keeps the letter readable
    // whether or not the bar underneath it is lit.
    const int labelHeight = juce::jmin(10, barHeight);
    if (labelHeight >= 8) {
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        for (int side = 0; side < (stereo ? 2 : 0); ++side) {
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
    const auto now = juce::Time::getMillisecondCounter();
    const int holdMs = peakHoldMs();

    const float peakL = juce::jlimit(0.0f, 1.0f, leftPeakSource.get());
    const float peakR = juce::jlimit(0.0f, 1.0f, rightPeakSource.get());
    displayedPeakL = juce::jmax(peakL, displayedPeakL * 0.85f);
    displayedPeakR = juce::jmax(peakR, displayedPeakR * 0.85f);

    displayedRmsL = smoothedRms(displayedRmsL, juce::jlimit(0.0f, 1.0f, leftRmsSource.get()));
    displayedRmsR = smoothedRms(displayedRmsR, juce::jlimit(0.0f, 1.0f, rightRmsSource.get()));

    // The held peak tracks the loudest value and, once its interval is up,
    // drops back to whatever the live peak is now and holds that for the next
    // interval.
    const auto updateHold = [now, holdMs](float& held, juce::uint32& heldAt, float peak) {
        if (peak >= held || now - heldAt >= static_cast<juce::uint32>(holdMs)) {
            held = peak;
            heldAt = now;
        }
    };
    updateHold(heldPeakL, holdStartL, displayedPeakL);
    updateHold(heldPeakR, holdStartR, displayedPeakR);

    repaint();
}