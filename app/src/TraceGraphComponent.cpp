#include "TraceGraphComponent.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr float kMinDb = -60.0f;
constexpr float kMaxDb = 12.0f;
constexpr double kMinFrequency = 20.0;
constexpr double kMaxFrequency = 20000.0;

constexpr std::array<double, 10> kGridFrequencies{
    20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0
};

juce::String frequencyText(double hz) {
    if (hz >= 1000.0)
        return juce::String(hz / 1000.0, hz >= 10000.0 ? 0 : 1) + "k";
    return juce::String(hz, 0);
}

double weightingDbAt(SpectrumWeighting weighting, double hz) {
    if (weighting == SpectrumWeighting::Z || hz <= 0.0) return 0.0;
    const double f2 = hz * hz;
    constexpr double kF1 = 20.598997 * 20.598997;
    constexpr double kF2 = 107.65265 * 107.65265;
    constexpr double kF3 = 737.86223 * 737.86223;
    constexpr double kF4 = 12194.217 * 12194.217;
    if (weighting == SpectrumWeighting::A) {
        const double response = (kF4 * f2 * f2)
                                / ((f2 + kF1) * std::sqrt((f2 + kF2) * (f2 + kF3)) * (f2 + kF4));
        return 20.0 * std::log10(response) + 2.00;
    }
    const double response = (kF4 * f2) / ((f2 + kF1) * (f2 + kF4));
    return 20.0 * std::log10(response) + 0.06;
}

} // namespace

float weightingDb(SpectrumWeighting weighting, double hz) {
    return static_cast<float>(weightingDbAt(weighting, hz));
}

float displayRtaLevelDb(float measuredDb, float displayOffsetDb, SpectrumWeighting weighting, double hz) {
    if (!std::isfinite(measuredDb)) return kMinDb;
    constexpr double kRtaKneeDb = -12.0;
    constexpr double kRtaCompSlope = 0.6;
    double db = static_cast<double>(measuredDb)
        + static_cast<double>(displayOffsetDb)
        + weightingDbAt(weighting, hz);
    if (db < kRtaKneeDb) db = kRtaKneeDb + (db - kRtaKneeDb) * kRtaCompSlope;
    // True digital silence (-inf / -100 dBFS FFT floor) eases to the bottom
    // instead of snapping to it, so quiet-but-real signal between -36 and
    // -70 dBFS no longer floors to -60. A hard threshold would hide real
    // low-level program content; the soft roll keeps silence at the floor
    // while letting -40..-60 dBFS traces land mid-graph.
    if (measuredDb <= kTraceRtaSilenceFloorDb + 30.0f) {
        const double t = std::clamp((static_cast<double>(measuredDb) - static_cast<double>(kTraceRtaSilenceFloorDb))
                                    / 30.0, 0.0, 1.0);
        db = kMinDb + (db - kMinDb) * t;
    }
    return static_cast<float>(db);
}

float clampDb(float db) {
    return std::clamp(std::isfinite(db) ? db : kMinDb, kMinDb, kMaxDb);
}

float dbToY(float db, juce::Rectangle<float> graph) {
    const float t = (clampDb(db) - kMinDb) / (kMaxDb - kMinDb);
    return graph.getBottom() - t * graph.getHeight();
}

float frequencyToX(double hz, juce::Rectangle<float> graph) {
    const double position = std::log(std::clamp(hz, kMinFrequency, kMaxFrequency) / kMinFrequency)
        / std::log(kMaxFrequency / kMinFrequency);
    return graph.getX() + static_cast<float>(position) * graph.getWidth();
}

void drawFrame(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Rectangle<float> graph,
               bool frequencyScale) {
    g.fillAll(theme::bg());
    g.setColour(theme::control());
    g.fillRoundedRectangle(bounds.reduced(1.0f), 4.0f);
    g.setColour(theme::border());
    g.drawRoundedRectangle(bounds.reduced(1.0f), 4.0f, 1.0f);

    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    if (frequencyScale) {
        for (const auto frequency : kGridFrequencies) {
            const auto x = frequencyToX(frequency, graph);
            g.setColour(theme::line().withAlpha(frequency == 1000.0 ? 0.65f : 0.32f));
            g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
            g.setColour(theme::dim());
            g.drawText(frequencyText(frequency), juce::roundToInt(x - 20.0f),
                       juce::roundToInt(graph.getBottom() + 4.0f), 40, 12,
                       juce::Justification::centred);
        }
    } else {
        for (int division = 0; division <= 4; ++division) {
            const auto x = graph.getX() + graph.getWidth() * static_cast<float>(division) / 4.0f;
            g.setColour(theme::line().withAlpha(division == 0 ? 0.65f : 0.3f));
            g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
        }
        g.setColour(theme::dim());
        g.drawText("HISTORY", juce::roundToInt(graph.getRight() - 54.0f),
                   juce::roundToInt(graph.getBottom() + 4.0f), 54, 12,
                   juce::Justification::right);
    }

    for (int db = -60; db <= 12; db += 12) {
        const auto y = dbToY(static_cast<float>(db), graph);
        g.setColour(theme::line().withAlpha(db == 0 ? 0.65f : 0.3f));
        g.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
        g.setColour(theme::dim());
        g.drawText(juce::String(db), 2, juce::roundToInt(y - 6.0f), 28, 12,
                   juce::Justification::right);
    }
}

void drawFrequencyTrace(juce::Graphics& g,
                        const std::vector<float>& values,
                        juce::Rectangle<float> graph,
                        juce::Colour colour,
                        bool fill) {
    if (values.empty()) return;
    juce::Path path;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto x = graph.getX() + graph.getWidth()
            * static_cast<float>(i) / static_cast<float>(std::max<std::size_t>(1, values.size() - 1));
        const auto y = dbToY(values[i], graph);
        if (i == 0) path.startNewSubPath(x, y);
        else path.lineTo(x, y);
    }
    if (fill) {
        path.lineTo(graph.getRight(), graph.getBottom());
        path.lineTo(graph.getX(), graph.getBottom());
        path.closeSubPath();
        g.setColour(colour.withAlpha(0.10f));
        g.fillPath(path);
    }
    g.setColour(colour.withAlpha(0.9f));
    g.strokePath(path, juce::PathStrokeType(1.8f));
}

void TraceGraphComponent::drawSpectrum(juce::Graphics& g,
                                       juce::Rectangle<float> graph,
                                       const std::vector<float>& values,
                                       double sampleRateHz,
                                       float displayOffsetDb,
                                       SpectrumWeighting weighting) {
    if (values.empty()) return;
    juce::Path path;
    for (int x = 0; x <= graph.getWidth(); ++x) {
        const auto position = static_cast<float>(x) / graph.getWidth();
        const double frequency = kMinFrequency
            * std::pow(kMaxFrequency / kMinFrequency, static_cast<double>(position));
        const auto level = TraceGraphComponent::spectrumLevelAtDb(values, frequency, sampleRateHz);
        const auto y = dbToY(displayRtaLevelDb(level, displayOffsetDb, weighting, frequency), graph);
        const auto px = graph.getX() + static_cast<float>(x);
        if (x == 0) path.startNewSubPath(px, y);
        else path.lineTo(px, y);
    }
    path.lineTo(graph.getRight(), graph.getBottom());
    path.lineTo(graph.getX(), graph.getBottom());
    path.closeSubPath();
    g.setColour(theme::text().withAlpha(0.10f));
    g.fillPath(path);
    g.setColour(theme::text().withAlpha(0.9f));
    g.strokePath(path, juce::PathStrokeType(1.8f));
}

void drawTimeTrace(juce::Graphics& g,
                   const std::vector<float>& values,
                   juce::Rectangle<float> graph,
                   juce::Colour colour,
                   bool fill) {
    if (values.empty()) return;
    juce::Path path;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto x = graph.getX() + graph.getWidth()
            * static_cast<float>(i) / static_cast<float>(std::max<std::size_t>(1, values.size() - 1));
        const auto y = dbToY(values[i], graph);
        if (i == 0) path.startNewSubPath(x, y);
        else path.lineTo(x, y);
    }
    if (fill) {
        path.lineTo(graph.getRight(), graph.getBottom());
        path.lineTo(graph.getX(), graph.getBottom());
        path.closeSubPath();
        g.setColour(colour.withAlpha(0.10f));
        g.fillPath(path);
    }
    g.setColour(colour.withAlpha(0.9f));
    g.strokePath(path, juce::PathStrokeType(1.8f));
}

juce::Rectangle<float> TraceGraphComponent::miniGraphBounds(juce::Rectangle<int> bounds) {
    // Full-width previews with guaranteed-cleared labels: the graph spans
    // the card edge to edge horizontally (only the 2px card border), with
    // 12px reserved at the bottom for the frequency labels. Callers draw
    // those labels *inside* that strip (graph bottom + 1px), never below
    // the card, so they can't be half-clipped by the border.
    auto graph = bounds.toFloat().reduced(2.0f, 2.0f);
    graph.removeFromBottom(12.0f);
    return graph;
}

float TraceGraphComponent::miniTraceDb(float db, juce::Rectangle<float> graph) {
    return dbToY(db, graph);
}

juce::Colour TraceGraphComponent::miniTraceColour(float, float) {
    return theme::amber();
}

void TraceGraphComponent::drawMiniTrace(juce::Graphics& g,
                                          juce::Rectangle<float> graph,
                                          const std::vector<float>& values,
                                          juce::Colour colour,
                                          bool fill) {
    drawFrequencyTrace(g, values, graph, colour, fill);
}

void drawLegend(juce::Graphics& g,
                juce::Rectangle<float> bounds,
                const std::vector<std::pair<const char*, juce::Colour>>& entries) {
    float x = bounds.getX() + 8.0f;
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    for (const auto& [label, colour] : entries) {
        g.setColour(colour);
        g.drawText(label, juce::roundToInt(x), juce::roundToInt(bounds.getY() + 4.0f),
                   92, 14, juce::Justification::left);
        x += 96.0f;
    }
}

double TraceGraphComponent::spectrumHzAtBin(std::size_t bin, std::size_t binCount, double sampleRateHz) {
    if (binCount <= 1 || sampleRateHz <= 0.0) return 0.0;
    return static_cast<double>(bin)
        * (sampleRateHz * 0.5)
        / static_cast<double>(binCount - 1);
}

float TraceGraphComponent::spectrumLevelAtDb(const std::vector<float>& values,
                                             double frequencyHz,
                                             double sampleRateHz) {
    if (values.size() < 2 || sampleRateHz <= 0.0) return kMinDb;
    const double nyquist = sampleRateHz * 0.5;
    const double last = static_cast<double>(values.size() - 1);
    const double pos = juce::jlimit(0.0, last, frequencyHz / nyquist * last);
    const std::size_t low = static_cast<std::size_t>(pos);
    const std::size_t high = juce::jmin(low + 1, values.size() - 1);
    const double frac = pos - static_cast<double>(low);
    return static_cast<float>(static_cast<double>(values[low]) * (1.0 - frac)
                              + static_cast<double>(values[high]) * frac);
}

TraceGraphComponent::TraceGraphComponent() {
    setOpaque(true);
}

void TraceGraphComponent::setFrequencyData(FrequencyTraceData data) {
    frequencyData_ = std::move(data);
    repaint();
}

void TraceGraphComponent::setTimeData(TimeTraceData data) {
    timeData_ = std::move(data);
    repaint();
}

void TraceGraphComponent::setMode(TraceGraphMode newMode) {
    if (mode_ == newMode) return;
    mode_ = newMode;
    repaint();
}

void TraceGraphComponent::setOverlayPainter(
    std::function<void(juce::Graphics&, juce::Rectangle<float>)> painter) {
    overlayPainter_ = std::move(painter);
    repaint();
}

juce::Rectangle<float> TraceGraphComponent::graphBounds(juce::Rectangle<int> bounds) {
    auto graph = bounds.toFloat().reduced(36.0f, 18.0f);
    graph.removeFromBottom(20.0f);
    graph.removeFromLeft(30.0f);
    graph.removeFromRight(18.0f);
    return graph;
}

void TraceGraphComponent::drawFrequency(
    juce::Graphics& g,
    juce::Rectangle<float> bounds,
    const FrequencyTraceData& data,
    const std::function<void(juce::Graphics&, juce::Rectangle<float>)>& overlay) {
    const auto graph = graphBounds(bounds.toNearestInt());
    drawFrame(g, bounds, graph, true);
    drawFrequencyTrace(g, data.targetDb, graph, theme::cyan(), true);
    drawSpectrum(g, graph, data.activityDb, data.spectrumSampleRateHz,
                 data.spectrumDisplayOffsetDb, data.spectrumWeighting);
    drawFrequencyTrace(g, data.reductionDb, graph, theme::green(), false);
    if (overlay) overlay(g, graph);
}

void TraceGraphComponent::drawTime(
    juce::Graphics& g,
    juce::Rectangle<float> bounds,
    const TimeTraceData& data,
    const std::function<void(juce::Graphics&, juce::Rectangle<float>)>& overlay) {
    const auto graph = graphBounds(bounds.toNearestInt());
    drawFrame(g, bounds, graph, false);
    drawTimeTrace(g, data.inputDb, graph, theme::amber(), true);
    drawTimeTrace(g, data.outputDb, graph, theme::blue(), false);
    drawTimeTrace(g, data.reductionDb, graph, theme::red(), false);
    if (overlay) overlay(g, graph);
    drawLegend(g, bounds, {{"INPUT", theme::amber()}, {"GAIN REDUCTION", theme::red()}, {"OUTPUT", theme::blue()}});
}

void TraceGraphComponent::paint(juce::Graphics& g) {
    if (mode_ == TraceGraphMode::Frequency)
        drawFrequency(g, getLocalBounds().toFloat(), frequencyData_, overlayPainter_);
    else
        drawTime(g, getLocalBounds().toFloat(), timeData_, overlayPainter_);
}
