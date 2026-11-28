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
    if (db <= LevelMeter::kMinDb) return juce::String("-Inf");
    return juce::String(juce::jmin(db, LevelMeter::kMaxDb), 1);
}

juce::String intervalText(int ms) {
    return ms >= 1000 ? juce::String(ms / 1000) + " s" : juce::String(ms) + " ms";
}

// Loudness arrives in LUFS already (dB relative to full scale), so unlike the
// peak/RMS rows there is no linear-to-dB step.
juce::String loudnessText(float lufs) {
    if (lufs <= -69.5f) return juce::String("-inf");
    return juce::String(lufs, 1);
}

// Below this the meter is silent: a decayed bar approaches zero but never
// reaches it, and a value this small is far under one pixel and under the
// 0.1 dB step of the readouts. Snapping it to 0 lets a quiet meter stop
// repainting, and the same number is the "did anything move" threshold.
constexpr float kRepaintEpsilon = 1.0e-5f;

// A laid-out label plus the natural bounds it was measured at.
struct TextArrangement {
    juce::GlyphArrangement glyphs;
    juce::Rectangle<float> bounds;
};

TextArrangement makeLabel(const juce::String& text) {
    TextArrangement made;
    made.glyphs.addLineOfText(juce::Font(juce::FontOptions(7.0f)), text, 0.0f, 0.0f);
    made.bounds = made.glyphs.getBoundingBox(0, -1, true);
    return made;
}

// "Peak" and "RMS" are constant text in one constant font, so their glyph runs
// are laid out once for the process rather than on every frame of every meter.
const TextArrangement& peakNameLabel() {
    static const TextArrangement label = makeLabel("Peak");
    return label;
}

const TextArrangement& rmsNameLabel() {
    static const TextArrangement label = makeLabel("RMS");
    return label;
}

const TextArrangement& lufsNameLabel() {
    static const TextArrangement label = makeLabel("LUFS");
    return label;
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
                       juce::Atomic<float>& rightRms, juce::Atomic<float>* loudness)
    : fader(faderToFollow),
      leftPeakSource(leftPeak),
      rightPeakSource(rightPeak),
      leftRmsSource(leftRms),
      rightRmsSource(rightRms),
      loudnessSource(loudness) {
    // The strip may be built while audio is already running; start from the
    // live values instead of an empty bar.
    displayedPeakL = heldPeakL = leftPeakSource.get();
    displayedPeakR = heldPeakR = rightPeakSource.get();
    displayedRmsL = leftRmsSource.get();
    displayedRmsR = rightRmsSource.get();
    if (loudnessSource != nullptr) displayedLoudness = loudnessSource->get();
    setTooltip("Peak, RMS, momentary loudness (LUFS) and the held peak "
               "(interval is set in Settings)");
    startTimerHz(30);
}

int LevelMeter::peakHoldMs() {
    return peakHoldMsValue.load(std::memory_order_relaxed);
}

void LevelMeter::setPeakHoldMs(int ms) {
    peakHoldMsValue.store(juce::jlimit(100, 1000, ms), std::memory_order_relaxed);
}

juce::String LevelMeter::peakHoldLabel(int ms) {
    return intervalText(ms);
}

void LevelMeter::setStereo(bool shouldBeStereo) {
    if (stereo == shouldBeStereo) return;
    stereo = shouldBeStereo;
    // Which reading the row shows changed (max of both sides vs left), so the
    // cached numeric glyph runs are no longer valid.
    peakGlyphs.text.clear();
    rmsGlyphs.text.clear();
    repaint();
}

void LevelMeter::refreshValueGlyphs(ValueGlyphs& cache, const juce::String& text) {
    if (cache.text == text) return;
    cache.text = text;
    cache.glyphs.clear();
    cache.glyphs.addLineOfText(theme::ui::numberFont(9.0f), text, 0.0f, 0.0f);
    cache.bounds = cache.glyphs.getBoundingBox(0, -1, true);
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

    const int readoutHeight = juce::jmin(33, barTop - well.getY() - 1);
    if (readoutHeight >= 16) {
        auto readouts = juce::Rectangle<int>(well.getX() + 2, barTop - readoutHeight - 1,
                                             well.getWidth() - 4, readoutHeight);
        const bool loudnessMode =
            loudnessSource != nullptr && readoutMode() == Readout::Lufs;
        // Two rows in both modes: peak on top, then RMS or momentary loudness.
        // Allocating three rows left a blank gap in LUFS mode.
        const int rowHeight = readoutHeight / 2;
        auto peakRow = readouts.removeFromTop(rowHeight);
        // Glyphs are drawn at their natural size and placed by their own
        // bounds: a fixed rect either squeezes a wide value or clips its last
        // digit on this narrow meter. Both arrangements are cached — the label
        // is constant, the value only changes with its 0.1 dB text.
        const auto drawReadout = [&g](juce::Rectangle<int> row, const ValueGlyphs& value,
                                      const TextArrangement& name) {
            const int nameRight = row.getRight();
            const int valueRight = nameRight - juce::roundToInt(name.bounds.getWidth()) - 3;
            const auto place = [&row](const juce::Rectangle<float>& bounds, int right) {
                return juce::AffineTransform::translation(
                    (float)right - bounds.getRight(),
                    (float)row.getY()
                        + ((float)row.getHeight() - bounds.getHeight()) * 0.5f - bounds.getY());
            };

            g.setColour(theme::text());
            value.glyphs.draw(g, place(value.bounds, valueRight));
            g.setColour(theme::dim());
            name.glyphs.draw(g, place(name.bounds, nameRight));
        };
        const float loudestPeak = stereo ? juce::jmax(displayedPeakL, displayedPeakR) : displayedPeakL;
        const float loudestRms = stereo ? juce::jmax(displayedRmsL, displayedRmsR) : displayedRmsL;
        refreshValueGlyphs(peakGlyphs, dbText(loudestPeak));
        refreshValueGlyphs(rmsGlyphs, dbText(loudestRms));
        drawReadout(peakRow, peakGlyphs, peakNameLabel());
        auto rmsRow = readouts.removeFromTop(rowHeight);
        if (loudnessMode) {
            refreshValueGlyphs(lufsGlyphs, loudnessText(displayedLoudness));
            drawReadout(rmsRow, lufsGlyphs, lufsNameLabel());
        } else {
            drawReadout(rmsRow, rmsGlyphs, rmsNameLabel());
        }
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
    g.setFont(theme::ui::numberFont(8.0f));
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

    // Each bar is named at its foot, straight over the bar and with no plate
    // behind it: any backing turns the letters into a small level display of
    // their own, and any band or frame there reads as one.
    const int labelHeight = juce::jmin(10, barHeight);
    if (stereo && labelHeight >= 8) {
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        for (int side = 0; side < 2; ++side) {
            const auto bar = side == 0 ? leftBar : rightBar;
            g.setColour(theme::text());
            g.drawText(side == 1 ? "R" : "L",
                       bar.withY(bar.getBottom() - labelHeight).withHeight(labelHeight),
                       juce::Justification::centred, false);
        }
    }

    g.setColour(theme::line().withAlpha(0.65f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 2.0f, 1.0f);
}

void LevelMeter::timerCallback() {
    const auto now = juce::Time::getMillisecondCounter();
    const int holdMs = peakHoldMs();

    const float previousPeakL = displayedPeakL;
    const float previousPeakR = displayedPeakR;
    const float previousRmsL = displayedRmsL;
    const float previousRmsR = displayedRmsR;
    const float previousHeldL = heldPeakL;
    const float previousHeldR = heldPeakR;
    const float previousLoudness = displayedLoudness;

    // A decayed bar approaches zero asymptotically; snap it to true silence so
    // a meter with no signal stops repainting instead of redrawing forever.
    const auto snapSilent = [](float value) {
        return value < kRepaintEpsilon ? 0.0f : value;
    };

    // Keep the full amplitude range (up to kMaxDb) so hot signals are not
    // clamped to 0 dB; proportionOf()/dbText() handle the display range.
    const float peakL = juce::jmax(0.0f, leftPeakSource.get());
    const float peakR = juce::jmax(0.0f, rightPeakSource.get());
    displayedPeakL = snapSilent(juce::jmax(peakL, displayedPeakL * 0.85f));
    displayedPeakR = snapSilent(juce::jmax(peakR, displayedPeakR * 0.85f));

    displayedRmsL = snapSilent(smoothedRms(displayedRmsL, juce::jmax(0.0f, leftRmsSource.get())));
    displayedRmsR = snapSilent(smoothedRms(displayedRmsR, juce::jmax(0.0f, rightRmsSource.get())));
    // The engine already averages loudness over 400 ms, so the readout takes
    // the published value as it is.
    if (loudnessSource != nullptr)
        displayedLoudness = juce::jlimit(-70.0f, 6.0f, loudnessSource->get());

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

    // A hidden meter needs no redraw, and a still one neither: compare in
    // linear amplitude, where kRepaintEpsilon is far below one drawn pixel and
    // below the 0.1 dB step of the readouts.
    if (!isShowing()) return;
    const auto moved = [](float after, float before) {
        return !(std::abs(after - before) < kRepaintEpsilon);
    };
    if (moved(displayedPeakL, previousPeakL) || moved(displayedPeakR, previousPeakR)
        || moved(displayedRmsL, previousRmsL) || moved(displayedRmsR, previousRmsR)
        || moved(heldPeakL, previousHeldL) || moved(heldPeakR, previousHeldR)
        || std::abs(displayedLoudness - previousLoudness) >= 0.05f)
        repaint();
}