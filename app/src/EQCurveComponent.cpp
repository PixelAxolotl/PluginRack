#include "EQCurveComponent.h"

#include <cmath>

#include "Theme.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kNodeRadius = 6;     // node dot radius, px
constexpr int kNodeHitRadius = 10; // mouseDown pick tolerance, px
constexpr int kCurveStepPx = 2;    // magnitude sampling pitch, px

// Clamps mirror core/EqEngine.cpp's computeCoeffs so the drawn response always
// matches what the engine will actually apply.
constexpr double kCoreMinFreqHz = 10.0;
constexpr double kCoreMinQ = 0.1;
constexpr double kCoreMaxQ = 20.0;
constexpr double kButterworthQ = 0.7071067811865476; // HP ignores band.q

// Grid vertical lines: 1-2-5(ish) per decade over the 20 Hz - 20 kHz span.
constexpr double kGridFreqs[] = {
    20, 30, 50, 70, 100, 200, 300, 500, 700, 1000, 2000, 3000, 5000, 7000, 10000, 20000
};
// Frequency-axis text labels.
constexpr int kLabelFreqs[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};

bool paramsFinite(const pluginrack::EqBand& band) noexcept {
    return std::isfinite(static_cast<double>(band.freqHz))
        && std::isfinite(static_cast<double>(band.gainDb))
        && std::isfinite(static_cast<double>(band.q));
}

// True when EqEngine::rebuild would give this band a coefficient set.
bool bandActive(const pluginrack::EqBand& band, bool anySolo) noexcept {
    return band.enabled && paramsFinite(band) && (!anySolo || band.solo);
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
    repaint();
}

void EQCurveComponent::setSampleRateHz(double rateHz) {
    if (!(rateHz > 0.0)) return; // non-positive ignored: keep the fallback
    if (sampleRateHz == rateHz) return;
    sampleRateHz = rateHz;
    repaint();
}

void EQCurveComponent::setSelectedBand(int index) {
    if (index < 0 || index >= static_cast<int>(bands.size())) return;
    if (index == selectedBand) return;
    selectedBand = index;
    repaint();
}

juce::Colour EQCurveComponent::bandColour(int index) {
    switch (index) {
        case 0: return theme::red();    // high-pass
        case 1: return theme::green();  // low shelf
        case 5: return theme::violet(); // high shelf
        default: return theme::cyan();  // peak bands
    }
}

juce::Point<float> EQCurveComponent::nodePosition(const pluginrack::EqBand& band,
                                                  int width, int height) {
    const double t = std::log(band.freqHz / kMinFreqHz) / std::log(kMaxFreqHz / kMinFreqHz);
    const float x = static_cast<float>(juce::jlimit(0.0, 1.0, t) * width);
    // HP ignores gainDb, so its node rides the 0 dB line like the response does.
    const double gain = (band.type == pluginrack::EqType::HighPass)
                            ? 0.0
                            : juce::jlimit(-kMaxGainDb, kMaxGainDb, static_cast<double>(band.gainDb));
    const float y = static_cast<float>(height * 0.5 - gain / kMaxGainDb * height * 0.5);
    return {x, y};
}

double EQCurveComponent::bandShapeDb(const pluginrack::EqBand& band,
                                     double sampleRateHz, double hz) {
    // Normalised RBJ coefficients, identical maths to core/EqEngine.cpp
    // computeCoeffs() (clamping included so dragged params can never diverge
    // from what the engine applies).
    const double nyquist = sampleRateHz * 0.5;
    const double maxFreq = (kMaxFreqHz < nyquist) ? kMaxFreqHz : nyquist - 1.0;
    const double freq = std::fmin(std::fmax(static_cast<double>(band.freqHz), kCoreMinFreqHz), maxFreq);
    const double q = std::fmin(std::fmax(static_cast<double>(band.q), kCoreMinQ), kCoreMaxQ);
    const double gainDb = static_cast<double>(band.gainDb);

    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * freq / sampleRateHz;
    const double sinW0 = std::sin(w0);
    const double cosW0 = std::cos(w0);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (band.type) {
        case pluginrack::EqType::HighPass: {
            const double alpha = sinW0 / (2.0 * kButterworthQ);
            b0 = (1.0 + cosW0) * 0.5;
            b1 = -(1.0 + cosW0);
            b2 = (1.0 + cosW0) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW0;
            a2 = 1.0 - alpha;
            break;
        }
        case pluginrack::EqType::LowShelf: {
            const double alpha = sinW0 * 0.5 * std::sqrt((a + 1.0 / a) * (1.0 / (q * q) - 1.0) + 2.0);
            const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
            b0 = a * ((a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha);
            b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW0);
            b2 = a * ((a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha);
            a0 = (a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha;
            a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosW0);
            a2 = (a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha;
            break;
        }
        case pluginrack::EqType::Peak: {
            const double alpha = sinW0 / (2.0 * q);
            // RBJ cookbook bell: centre gain = a^2 = 10^(gainDb/20), which
            // requires the denominator 1 +/- alpha/a (1 +/- alpha halves it).
            b0 = 1.0 + alpha * a;
            b1 = -2.0 * cosW0;
            b2 = 1.0 - alpha * a;
            a0 = 1.0 + alpha / a;
            a1 = -2.0 * cosW0;
            a2 = 1.0 - alpha / a;
            break;
        }
        case pluginrack::EqType::HighShelf: {
            const double alpha = sinW0 * 0.5 * std::sqrt((a + 1.0 / a) * (1.0 / (q * q) - 1.0) + 2.0);
            const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
            b0 = a * ((a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha);
            b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosW0);
            b2 = a * ((a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha);
            a0 = (a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha;
            a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosW0);
            a2 = (a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha;
            break;
        }
    }

    const double inv = 1.0 / a0;
    b0 *= inv; b1 *= inv; b2 *= inv; a1 *= inv; a2 *= inv;

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

double EQCurveComponent::xToFreq(float x) const {
    const double t = juce::jlimit(0.0, 1.0, static_cast<double>(x) / static_cast<double>(juce::jmax(1, getWidth())));
    return kMinFreqHz * std::pow(kMaxFreqHz / kMinFreqHz, t);
}

float EQCurveComponent::freqToX(double hz) const {
    const double t = std::log(juce::jlimit(kMinFreqHz, kMaxFreqHz, hz) / kMinFreqHz)
                   / std::log(kMaxFreqHz / kMinFreqHz);
    return static_cast<float>(t * getWidth());
}

double EQCurveComponent::yToGain(float y) const {
    const double h = static_cast<double>(juce::jmax(1, getHeight()));
    return juce::jlimit(-kMaxGainDb, kMaxGainDb, (h * 0.5 - static_cast<double>(y)) / (h * 0.5) * kMaxGainDb);
}

float EQCurveComponent::gainToY(double gainDb) const {
    const double h = getHeight();
    return static_cast<float>(h * 0.5 - gainDb / kMaxGainDb * h * 0.5);
}

float EQCurveComponent::spectrumY(double hz) const {
    // Bin centres are linear from 0 Hz to nyquist of the reference rate.
    const double nyquist = sampleRateHz * 0.5;
    const double last = static_cast<double>(spectrumDb.size() - 1);
    const double pos = juce::jlimit(0.0, last, hz / nyquist * last);
    const std::size_t low = static_cast<std::size_t>(pos);
    const std::size_t high = juce::jmin(low + 1, spectrumDb.size() - 1);
    const double frac = pos - static_cast<double>(low);
    const double db = static_cast<double>(spectrumDb[low]) * (1.0 - frac)
                    + static_cast<double>(spectrumDb[high]) * frac;
    return gainToY(juce::jlimit(-kMaxGainDb, kMaxGainDb, db));
}

void EQCurveComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::strip());

    const int w = getWidth();
    const int h = getHeight();
    if (w < 40 || h < 40 || bands.empty()) return;

    // --- grid: 20 Hz - 20 kHz log X, +/-15 dB linear Y (0 dB line heavier).
    for (const double f : kGridFreqs) {
        const float x = freqToX(f);
        const bool edge = (f <= kMinFreqHz || f >= kMaxFreqHz);
        g.setColour(edge ? theme::border() : theme::border().withAlpha(0.35f));
        g.drawLine(x, 0.0f, x, static_cast<float>(h), edge ? 1.0f : 0.5f);
    }
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    for (const int f : kLabelFreqs) {
        const float x = freqToX(static_cast<double>(f));
        g.setColour(theme::dim().withAlpha(0.8f));
        const int edgeBias = (f <= kMinFreqHz) ? 20 : (f >= kMaxFreqHz ? -20 : 0);
        g.drawText(freqLabel(f), juce::Rectangle<int>(static_cast<int>(x) - 20 + edgeBias, h - 13, 40, 12),
                   juce::Justification::centred, false);
    }
    for (int db = -15; db <= 15; db += 3) {
        const float y = gainToY(static_cast<double>(db));
        g.setColour(db == 0 ? theme::line().withAlpha(0.8f) : theme::border().withAlpha(0.4f));
        g.drawLine(0.0f, y, static_cast<float>(w), y, db == 0 ? 1.0f : 0.5f);
    }

    // --- analyzer spectrum (Task 6): grey fill behind the curves, Pro-Q
    // look. Bin magnitudes arrive from EQWindow's UI-timer FFT; below the
    // view's -15 dB floor the fill simply rests on the bottom edge.
    if (!spectrumDb.empty()) {
        juce::Path spectrum;
        for (int x = 0; x <= w; x += kCurveStepPx) {
            const float y = spectrumY(xToFreq(static_cast<float>(x)));
            if (x == 0) spectrum.startNewSubPath(0.0f, y);
            else spectrum.lineTo(static_cast<float>(x), y);
        }
        spectrum.lineTo(static_cast<float>(w), static_cast<float>(h));
        spectrum.lineTo(0.0f, static_cast<float>(h));
        spectrum.closeSubPath();
        g.setColour(juce::Colours::grey.withAlpha(0.35f));
        g.fillPath(spectrum);
    }

    // --- combined magnitude (yellow): dB sum of the active bands' responses,
    // via the same magnitudeDbAt the sine-vs-curve verification checks.
    auto paintCurve = [&](int index, juce::Colour colour, float thickness, bool fill) {
        juce::Path curve;
        for (int x = 0; x <= w; x += kCurveStepPx) {
            const double f = xToFreq(static_cast<float>(x));
            const double totalDb = (index >= 0)
                ? bandShapeDb(bands[static_cast<std::size_t>(index)], sampleRateHz, f)
                : magnitudeDbAt(f);
            const float y = gainToY(juce::jlimit(-kMaxGainDb * 1.4, kMaxGainDb * 1.4, totalDb));
            if (x == 0) curve.startNewSubPath(static_cast<float>(x), y);
            else curve.lineTo(static_cast<float>(x), y);
        }
        if (fill) {
            juce::Path filled = curve;
            filled.lineTo(static_cast<float>(w), h * 0.5f);
            filled.lineTo(0.0f, h * 0.5f);
            filled.closeSubPath();
            g.setColour(colour.withAlpha(0.14f));
            g.fillPath(filled);
        }
        g.setColour(colour);
        g.strokePath(curve, juce::PathStrokeType(thickness));
    };

    paintCurve(-1, theme::amber(), 2.0f, true); // combined

    // Selected band alone, dashed in its band colour (drawn under the nodes).
    if (selectedBand >= 0 && selectedBand < static_cast<int>(bands.size()))
        paintCurve(selectedBand, bandColour(selectedBand).withAlpha(0.55f), 1.0f, false);

    // --- nodes (band colours; dimmed when bypassed, ringed when selected).
    for (int i = 0; i < static_cast<int>(bands.size()); ++i) {
        const pluginrack::EqBand& band = bands[static_cast<std::size_t>(i)];
        const juce::Point<float> p = nodePosition(band, w, h);
        g.setColour(bandColour(i).withAlpha(band.enabled ? 1.0f : 0.35f));
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

void EQCurveComponent::mouseDown(const juce::MouseEvent& e) {
    const int w = getWidth();
    const int h = getHeight();
    if (w < 40 || h < 40) return;

    // Pick the nearest node within tolerance.
    int best = -1;
    float bestDist = static_cast<float>(kNodeHitRadius * kNodeHitRadius);
    for (int i = 0; i < static_cast<int>(bands.size()); ++i) {
        const juce::Point<float> p = nodePosition(bands[static_cast<std::size_t>(i)], w, h);
        const float dx = e.position.getX() - p.x;
        const float dy = e.position.getY() - p.y;
        const float dist = dx * dx + dy * dy;
        if (dist <= bestDist) { bestDist = dist; best = i; }
    }
    if (best < 0) return;

    setSelectedBand(best);
    dragBand = best;
    // Selecting alone is not a band change, but the window must sync its knobs
    // to the new selection without pushing bands to the engine.
    if (onBandSelected) onBandSelected();
}

void EQCurveComponent::mouseDrag(const juce::MouseEvent& e) {
    if (dragBand < 0 || dragBand >= static_cast<int>(bands.size())) return;
    pluginrack::EqBand& band = bands[static_cast<std::size_t>(dragBand)];

    band.freqHz = static_cast<float>(xToFreq(e.position.getX()));
    if (band.type != pluginrack::EqType::HighPass) {
        band.gainDb = static_cast<float>(std::round(yToGain(e.position.getY()) * 10.0) / 10.0);
    }

    repaint();
    if (onBandsChanged) onBandsChanged();
}
