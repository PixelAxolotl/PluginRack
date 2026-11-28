#include "EQCurveComponent.h"

#include "AnalyzerConfig.h"

#include <cmath>

#include <pluginrack/DspMath.h>

#include "Theme.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kNodeRadius = 6;     // node dot radius, px
constexpr int kNodeHitRadius = 10; // mouseDown pick tolerance, px
constexpr int kCurveStepPx = 2;    // magnitude sampling pitch, px

// Grid vertical lines: 1-2-5(ish) per decade over the 20 Hz - 20 kHz span.
constexpr double kGridFreqs[] = {
    20, 30, 50, 70, 100, 200, 300, 500, 700, 1000, 2000, 3000, 5000, 7000, 10000, 20000
};
// Frequency-axis text labels: the same list every spectrum graph uses.
constexpr auto& kLabelFreqs = analyzer::kLabelFrequenciesHz;

bool paramsFinite(const pluginrack::EqBand& band) noexcept {
    return std::isfinite(static_cast<double>(band.freqHz))
        && std::isfinite(static_cast<double>(band.gainDb))
        && std::isfinite(static_cast<double>(band.q));
}

// True when EqEngine::rebuild would give this band a coefficient set.
bool bandActive(const pluginrack::EqBand& band, bool anySolo) noexcept {
    return band.enabled && paramsFinite(band) && (!anySolo || band.solo);
}

// The coefficient set paint() and magnitudeDbAt() both use, built once per
// band per curve rebuild instead of once per x-pixel.
pluginrack::dsp::Biquad coefficientSet(const pluginrack::EqBand& band, double sampleRateHz) {
    pluginrack::dsp::Biquad c{};
    switch (band.type) {
        case pluginrack::EqType::HighPass:
            c = pluginrack::dsp::highPass(band.freqHz, sampleRateHz,
                                          pluginrack::dsp::kButterworthQ);
            break;
        case pluginrack::EqType::LowPass:
            c = pluginrack::dsp::lowPass(band.freqHz, sampleRateHz,
                                         pluginrack::dsp::kButterworthQ);
            break;
        case pluginrack::EqType::LowShelf:
            c = pluginrack::dsp::lowShelf(band.freqHz, band.q, band.gainDb, sampleRateHz);
            break;
        case pluginrack::EqType::Peak:
            c = pluginrack::dsp::bell(band.freqHz, band.q, band.gainDb, sampleRateHz);
            break;
        case pluginrack::EqType::HighShelf:
            c = pluginrack::dsp::highShelf(band.freqHz, band.q, band.gainDb, sampleRateHz);
            break;
    }
    return c;
}

// |H(e^jw)| in dB for one coefficient set. Callers feed it a set they already
// validated with pluginrack::dsp::isFinite; a degenerate denominator still
// draws as no contribution, exactly as the old per-pixel path did.
double magnitudeDbFromCoeffs(const pluginrack::dsp::Biquad& c, double hz, double sampleRateHz) {
    const double b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;

    // |H(e^jw)| = |b0 + b1 z^-1 + b2 z^-2| / |1 + a1 z^-1 + a2 z^-2|.
    const double w = 2.0 * kPi * hz / sampleRateHz;
    const double cw = std::cos(w);
    const double sw = std::sin(w);
    const double c2w = std::cos(2.0 * w);
    const double s2w = std::sin(2.0 * w);
    const double numRe = b0 + b1 * cw + b2 * c2w;
    const double numIm = -(b1 * sw + b2 * s2w);
    const double denRe = 1.0 + a1 * cw + a2 * c2w;
    const double denIm = -(a1 * sw + a2 * s2w);

    const double numMag = std::sqrt(numRe * numRe + numIm * numIm);
    const double denMag = std::sqrt(denRe * denRe + denIm * denIm);
    if (!(denMag > 0.0)) return 0.0; // guarded against a degenerate denominator
    return 20.0 * std::log10(juce::jmax(1e-12, numMag / denMag));
}

juce::String freqLabel(int hz) {
    if (hz < 1000) return juce::String(hz);
    return juce::String(hz / 1000) + "k"; // label list only contains whole kHz
}

} // namespace

EQCurveComponent::EQCurveComponent() {
    setOpaque(true);
}

void EQCurveComponent::setBands(const std::vector<pluginrack::EqBand>& newBands) {
    bands = newBands;
    invalidateCurvePaths(); // enables/solos/freqs/gains/Q all ride in here
    repaint();
}

void EQCurveComponent::setSampleRateHz(double rateHz) {
    if (!(rateHz > 0.0)) return; // non-positive ignored: keep the fallback
    if (sampleRateHz == rateHz) return;
    sampleRateHz = rateHz;
    invalidateCurvePaths(); // every coefficient set depends on the rate
    repaint();
}

void EQCurveComponent::setSelectedBand(int index) {
    if (index < 0 || index >= static_cast<int>(bands.size())) return;
    if (index == selectedBand) return;
    selectedBand = index;
    invalidateCurvePaths(); // the dashed band path follows the selection
    repaint();
}

void EQCurveComponent::resized() {
    // Paths are in pixel coordinates: a new size invalidates all three.
    invalidateCurvePaths();
}

juce::Colour EQCurveComponent::bandColour(const pluginrack::EqBand& band) {
    switch (band.type) {
        case pluginrack::EqType::HighPass: return theme::red();    // low cut
        case pluginrack::EqType::LowPass: return theme::orange();  // high cut
        case pluginrack::EqType::LowShelf:
        case pluginrack::EqType::HighShelf: return theme::blue();  // shelves
        case pluginrack::EqType::Peak: break;
    }
    return theme::green(); // bells (band)
}

namespace {
// Filter points (high-pass / low-pass) carry no gain, so their node rides the
// 0 dB line like their response does.
bool isGainlessBand(const pluginrack::EqBand& band) noexcept {
    return band.type == pluginrack::EqType::HighPass
        || band.type == pluginrack::EqType::LowPass;
}
} // namespace

juce::Point<float> EQCurveComponent::nodePosition(const pluginrack::EqBand& band,
                                                  int width, int height) {
    const double t = analyzer::frequencyToFraction(band.freqHz);
    const float x = static_cast<float>(juce::jlimit(0.0, 1.0, t) * width);
    const double gain = isGainlessBand(band)
                            ? 0.0
                            : juce::jlimit(static_cast<double>(kMinDisplayDb), static_cast<double>(kMaxDisplayDb), static_cast<double>(band.gainDb));
    const float y = static_cast<float>(height
                                       * (1.0 - (gain - static_cast<double>(kMinDisplayDb))
                                              / (static_cast<double>(kMaxDisplayDb)
                                                 - static_cast<double>(kMinDisplayDb))));
    return {x, y};
}

double EQCurveComponent::bandShapeDb(const pluginrack::EqBand& band,
                                     double sampleRateHz, double hz) {
    // Same coefficients and cascade depth the engine applies: the RBJ maths now
    // lives once in core DspMath (the old local copy's shelf alpha went NaN
    // under the sqrt for Q > 1, so a shelved curve could diverge from what you
    // heard), and eqBandStageCount is the engine's own stage rule.
    const auto c = coefficientSet(band, sampleRateHz);
    // Mirrors EqEngine's non-finite band guard: a poisoned coefficient set
    // draws as no contribution instead of dragging the curve to NaN.
    if (!pluginrack::dsp::isFinite(c)) return 0.0;
    return static_cast<double>(pluginrack::eqBandStageCount(band)) *
           magnitudeDbFromCoeffs(c, hz, sampleRateHz);
}

double EQCurveComponent::magnitudeDbAt(double hz) const {
    // Same active-band rule and rate the yellow curve is painted with.
    bool anySolo = false;
    for (const auto& band : bands) {
        if (band.solo) { anySolo = true; break; }
    }
    double totalDb = 0.0;
    for (const auto& band : bands) {
        if (bandActive(band, anySolo)) totalDb += bandShapeDb(band, sampleRateHz, hz);
    }
    return totalDb;
}

void EQCurveComponent::setSpectrum(const float* magnitudeDb, int numBins) {
    if (magnitudeDb == nullptr || numBins <= 0) {
        if (spectrumDb.empty()) return;
        spectrumDb.clear();
    } else {
        spectrumDb.assign(magnitudeDb, magnitudeDb + static_cast<std::size_t>(numBins));
    }
    repaint();
}

void EQCurveComponent::setSpectrumOffsetDb(float offsetDb) {
    if (spectrumOffsetDb == offsetDb) return;
    spectrumOffsetDb = offsetDb;
    repaint();
}

void EQCurveComponent::setWeighting(Weighting newWeighting) {
    if (weighting == newWeighting) return;
    weighting = newWeighting;
    repaint();
}

double EQCurveComponent::xToFreq(float x) const {
    const double t = juce::jlimit(0.0, 1.0, static_cast<double>(x) / static_cast<double>(juce::jmax(1, getWidth())));
    return analyzer::fractionToFrequency(static_cast<float>(t));
}

float EQCurveComponent::freqToX(double hz) const {
    const double t = analyzer::frequencyToFraction(hz);
    return static_cast<float>(t * getWidth());
}

double EQCurveComponent::yToGain(float y) const {
    const double h = static_cast<double>(juce::jmax(1, getHeight()));
    return juce::jlimit(static_cast<double>(kMinDisplayDb), static_cast<double>(kMaxDisplayDb),
                        (1.0 - static_cast<double>(y) / h)
                            * (static_cast<double>(kMaxDisplayDb) - static_cast<double>(kMinDisplayDb))
                        + static_cast<double>(kMinDisplayDb));
}

float EQCurveComponent::gainToY(double gainDb) const {
    const double h = static_cast<double>(getHeight());
    const double displayDb = juce::jlimit(static_cast<double>(kMinDisplayDb),
                                          static_cast<double>(kMaxDisplayDb), gainDb);
    return static_cast<float>(h * (1.0 - (displayDb - static_cast<double>(kMinDisplayDb))
                                       / (static_cast<double>(kMaxDisplayDb)
                                          - static_cast<double>(kMinDisplayDb))));
}

float EQCurveComponent::spectrumY(double hz) const {
    return gainToY(displayRtaLevelDb(
        TraceGraphComponent::spectrumLevelAtDb(spectrumDb, hz, sampleRateHz),
        spectrumOffsetDb, weighting, hz));
}

void EQCurveComponent::rebuildCurvePaths() {
    curvePathsDirty = false;
    cachedCombinedPath.clear();
    cachedCombinedFill.clear();
    cachedSelectedPath.clear();
    cachedBandShapes.clear();

    const int w = getWidth();
    const int h = getHeight();
    if (w < 40 || h < 40 || bands.empty()) return; // nothing drawable yet

    // --- the active-band rule EqEngine::rebuild applies, evaluated once.
    bool anySolo = false;
    for (const auto& band : bands) {
        if (band.solo) { anySolo = true; break; }
    }

    cachedBandShapes.resize(bands.size());
    bool anyContribution = false;
    for (std::size_t i = 0; i < bands.size(); ++i) {
        if (!bandActive(bands[i], anySolo)) continue;
        const auto coeffs = coefficientSet(bands[i], sampleRateHz);
        if (!pluginrack::dsp::isFinite(coeffs)) continue; // draws no contribution
        cachedBandShapes[i].coeffs = coeffs;
        cachedBandShapes[i].stages = pluginrack::eqBandStageCount(bands[i]);
        cachedBandShapes[i].contributes = true;
        anyContribution = true;
    }

    // The selected band is drawn on its own even when bypassed or outside the
    // solo set (it has always been: only a non-finite set flattens it to 0 dB).
    const bool hasSelected = selectedBand >= 0 && selectedBand < static_cast<int>(bands.size());
    pluginrack::dsp::Biquad selectedCoeffs{};
    int selectedStages = 1;
    bool selectedFinite = false;
    if (hasSelected) {
        selectedCoeffs = coefficientSet(bands[static_cast<std::size_t>(selectedBand)], sampleRateHz);
        selectedStages = pluginrack::eqBandStageCount(bands[static_cast<std::size_t>(selectedBand)]);
        selectedFinite = pluginrack::dsp::isFinite(selectedCoeffs);
    }

    // --- sample the magnitude at the same pixel pitch paint() used to take.
    for (int x = 0; x <= w; x += kCurveStepPx) {
        const float px = static_cast<float>(x);
        const double freqHz = xToFreq(px);

        double totalDb = 0.0;
        if (anyContribution) {
            for (const auto& shape : cachedBandShapes) {
                if (shape.contributes)
                    totalDb += static_cast<double>(shape.stages)
                             * magnitudeDbFromCoeffs(shape.coeffs, freqHz, sampleRateHz);
            }
        }
        const float y = gainToY(totalDb);
        if (x == 0) cachedCombinedPath.startNewSubPath(px, y);
        else cachedCombinedPath.lineTo(px, y);

        if (hasSelected) {
            const double bandDb = selectedFinite
                ? static_cast<double>(selectedStages)
                      * magnitudeDbFromCoeffs(selectedCoeffs, freqHz, sampleRateHz)
                : 0.0;
            const float bandY = gainToY(bandDb);
            if (x == 0) cachedSelectedPath.startNewSubPath(px, bandY);
            else cachedSelectedPath.lineTo(px, bandY);
        }
    }

    // Fill under the combined curve: the closed copy paint() used to build.
    cachedCombinedFill = cachedCombinedPath;
    cachedCombinedFill.lineTo(static_cast<float>(w), gainToY(0.0));
    cachedCombinedFill.lineTo(0.0f, gainToY(0.0));
    cachedCombinedFill.closeSubPath();
}

void EQCurveComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::strip());

    const int w = getWidth();
    const int h = getHeight();
    if (w < 40 || h < 40 || bands.empty()) return;

    // Fonts and strokes never change between frames: build them once.
    static const juce::Font labelFont{juce::FontOptions(9.0f)};
    static const juce::Font dbFont{juce::FontOptions(8.0f)};
    static const juce::PathStrokeType combinedStroke{2.0f};
    static const juce::PathStrokeType selectedStroke{1.0f};

    // --- grid: 20 Hz - 20 kHz log X, +/-15 dB linear Y (0 dB line heavier).
    for (const double f : kGridFreqs) {
        const float x = freqToX(f);
        const bool edge = (f <= kMinFreqHz || f >= kMaxFreqHz);
        g.setColour(edge ? theme::border() : theme::border().withAlpha(0.35f));
        g.drawLine(x, 0.0f, x, static_cast<float>(h), edge ? 1.0f : 0.5f);
    }
    g.setFont(labelFont);
    for (const int f : kLabelFreqs) {
        const float x = freqToX(static_cast<double>(f));
        g.setColour(theme::dim().withAlpha(0.8f));
        // Keep the label boxes fully inside the card: the outer labels were
        // centred on their gridline, so left-half and right-half of "20" /
        // "20k" were half-clipped by the card border.
        const int labelX = juce::jlimit(0, juce::jmax(0, w - 40), static_cast<int>(x) - 20);
        g.drawText(freqLabel(f), juce::Rectangle<int>(labelX, h - 13, 40, 12),
                   juce::Justification::centred, false);
    }
    for (int db = static_cast<int>(analyzer::kMinDb); db <= static_cast<int>(analyzer::kMaxDb); db += analyzer::kDbGridStep) {
        const float y = gainToY(static_cast<double>(db));
        g.setColour(db == 0 ? theme::line().withAlpha(0.8f) : theme::border().withAlpha(0.4f));
        g.drawLine(0.0f, y, static_cast<float>(w), y, db == 0 ? 1.0f : 0.5f);
        g.setColour(theme::dim().withAlpha(0.9f));
        g.fillRect(0.0f, y - 0.5f, 6.0f, 1.0f);
        g.setFont(dbFont);
        // Clamped inside the component: the top and bottom marks used to be
        // half-drawn outside the curve's bounds.
        const int labelY = juce::jlimit(0, juce::jmax(0, h - 12), static_cast<int>(y) - 6);
        g.drawText((db > 0 ? "+" : "") + juce::String(db) + " dB",
                   juce::Rectangle<int>(2, labelY, 34, 12),
                   juce::Justification::left, false);
    }

    // --- analyzer spectrum: the shared TraceGraphComponent RTA renderer
    // (same path as Multiband/De-esser) — one RTA implementation for all
    // three frequency graphs, so style, offset, weighting and the silence
    // floor can never diverge again.
    if (!spectrumDb.empty()) {
        const juce::Rectangle<float> spectrumGraph(0.0f, 0.0f,
                                                   static_cast<float>(w), static_cast<float>(h));
        TraceGraphComponent::drawSpectrum(g, spectrumGraph, spectrumDb, sampleRateHz,
                                          spectrumOffsetDb, weighting);
    }

    // --- combined magnitude (yellow): dB sum of the active bands' responses.
    // The geometry (one biquad coefficient set per band, sampled at the same
    // pixel pitch) is rebuilt only when curvePathsDirty is set; magnitudeDbAt
    // answers the same maths for verification.
    if (curvePathsDirty) rebuildCurvePaths();

    if (!cachedCombinedPath.isEmpty()) {
        if (!cachedCombinedFill.isEmpty()) {
            g.setColour(theme::amber().withAlpha(0.14f));
            g.fillPath(cachedCombinedFill);
        }
        g.setColour(theme::amber()); // combined
        g.strokePath(cachedCombinedPath, combinedStroke);
    }

    // Selected band alone in its band colour, drawn under the nodes.
    if (selectedBand >= 0 && selectedBand < static_cast<int>(bands.size())
        && !cachedSelectedPath.isEmpty()) {
        g.setColour(bandColour(bands[static_cast<std::size_t>(selectedBand)]).withAlpha(0.55f));
        g.strokePath(cachedSelectedPath, selectedStroke);
    }

    // --- nodes (band colours; dimmed when bypassed, ringed when selected).
    for (int i = 0; i < static_cast<int>(bands.size()); ++i) {
        const pluginrack::EqBand& band = bands[static_cast<std::size_t>(i)];
        const juce::Point<float> p = nodePosition(band, w, h);
        g.setColour(bandColour(band).withAlpha(band.enabled ? 1.0f : 0.35f));
        g.fillEllipse(p.x - static_cast<float>(kNodeRadius), p.y - static_cast<float>(kNodeRadius),
                      static_cast<float>(kNodeRadius * 2), static_cast<float>(kNodeRadius * 2));
        if (i == selectedBand) {
            g.setColour(theme::text());
            g.drawEllipse(p.x - kNodeRadius - 2.5f, p.y - kNodeRadius - 2.5f,
                          (kNodeRadius + 2.5f) * 2.0f, (kNodeRadius + 2.5f) * 2.0f, 1.0f);
        }
        if (band.solo) {
            g.setColour(theme::amber());
            g.drawEllipse(p.x - kNodeRadius - 1.5f, p.y - kNodeRadius - 1.5f,
                          (kNodeRadius + 1.5f) * 2.0f, (kNodeRadius + 1.5f) * 2.0f, 1.5f);
        }
    }
}

int EQCurveComponent::bandIndexAt(juce::Point<float> position) const {
    const int w = getWidth();
    const int h = getHeight();
    if (w < 40 || h < 40) return -1;

    // Nearest node within the pick tolerance (closest wins on overlap).
    int best = -1;
    float bestDist = static_cast<float>(kNodeHitRadius * kNodeHitRadius);
    for (int i = 0; i < static_cast<int>(bands.size()); ++i) {
        const juce::Point<float> p = nodePosition(bands[static_cast<std::size_t>(i)], w, h);
        const float dx = position.getX() - p.x;
        const float dy = position.getY() - p.y;
        const float dist = dx * dx + dy * dy;
        if (dist <= bestDist) { bestDist = dist; best = i; }
    }
    return best;
}

void EQCurveComponent::mouseDown(const juce::MouseEvent& e) {
    const int best = bandIndexAt(e.position);
    if (best < 0) return;

    setSelectedBand(best);
    dragBand = best;
    // Selecting alone is not a band change, but the window must sync its knobs
    // to the new selection without pushing bands to the engine.
    if (onBandSelected) onBandSelected();
}

void EQCurveComponent::mouseDoubleClick(const juce::MouseEvent& e) {
    const int index = bandIndexAt(e.position);
    if (index < 0) return;
    const auto defaults = pluginrack::defaultEqBands();
    if (index >= static_cast<int>(defaults.size())) return;

    const auto& factory = defaults[static_cast<std::size_t>(index)];
    pluginrack::EqBand& band = bands[static_cast<std::size_t>(index)];
    if (band == factory) return;

    band = factory; // original type, frequency, gain, Q, enabled and solo
    dragBand = -1;
    setSelectedBand(index);
    invalidateCurvePaths(); // the band change landed directly in the vector
    repaint();
    if (onBandsChanged) onBandsChanged();
}

void EQCurveComponent::mouseDrag(const juce::MouseEvent& e) {
    if (dragBand < 0 || dragBand >= static_cast<int>(bands.size())) return;
    pluginrack::EqBand& band = bands[static_cast<std::size_t>(dragBand)];

    band.freqHz = static_cast<float>(xToFreq(e.position.getX()));
    if (!isGainlessBand(band)) {
        band.gainDb = static_cast<float>(std::round(yToGain(e.position.getY()) * 10.0) / 10.0);
    }

    invalidateCurvePaths(); // band moved under paint's feet
    repaint();
    if (onBandsChanged) onBandsChanged();
}
