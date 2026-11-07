#include "MultibandCurveComponent.h"

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {
constexpr float kMinDb = -48.0f;
constexpr float kMaxDb = 12.0f;
constexpr int kCrossoverHitRadius = 14;
constexpr float kCrossoverHandleRadius = 7.0f;

juce::String frequencyText(double hz) {
    if (hz >= 1000.0) return juce::String(hz / 1000.0, hz >= 10000.0 ? 0 : 1) + "k";
    return juce::String(hz, 0);
}
} // namespace

MultibandCurveComponent::MultibandCurveComponent() {
    setOpaque(true);
    normaliseEffect(effect);
}

void MultibandCurveComponent::normaliseEffect(pluginrack::MultibandEffect& value) {
    if (value.bands.empty()) value = pluginrack::makeDefaultMultibandEffect(value.id);
    const auto crossoverCount = value.bands.size() - 1;
    value.crossovers.resize(crossoverCount);

    float previous = static_cast<float>(kMinFreqHz);
    for (std::size_t i = 0; i < crossoverCount; ++i) {
        const float remaining = static_cast<float>(crossoverCount - i - 1);
        const float fallback = previous + (static_cast<float>(kMaxFreqHz) - previous)
            / static_cast<float>(crossoverCount - i);
        const float valueHz = std::isfinite(value.crossovers[i]) ? value.crossovers[i] : fallback;
        const float maxHz = static_cast<float>(kMaxFreqHz) - remaining * kMinBandWidthHz;
        value.crossovers[i] = juce::jlimit(previous + kMinBandWidthHz, maxHz, valueHz);
        previous = value.crossovers[i];
    }
}

float MultibandCurveComponent::compressionGainDb(float inputDb,
                                                   const pluginrack::MultibandBand& band) {
    if (band.bypassed || !band.enabled || band.ratio <= 1.0f) return 0.0f;
    const float distance = (inputDb + band.inputGainDb) - band.thresholdDb;
    const float knee = std::max(0.0f, band.kneeDb);
    if (knee > 0.0f && distance > -knee * 0.5f && distance < knee * 0.5f) {
        const float width = distance + knee * 0.5f;
        return (1.0f / band.ratio - 1.0f) * width * width / (2.0f * knee);
    }
    if (distance > 0.0f)
        return distance * (1.0f / band.ratio - 1.0f);
    return band.mode == pluginrack::MultibandMode::Expand
        ? std::min(0.0f, distance * (band.ratio - 1.0f))
        : 0.0f;
}

void MultibandCurveComponent::setEffect(const pluginrack::MultibandEffect& newEffect) {
    effect = newEffect;
    normaliseEffect(effect);
    selectedBand = juce::jlimit(0, static_cast<int>(effect.bands.size()) - 1, selectedBand);
    targetTraceDb = pluginrack::makeMultibandTargetTrace(effect, 256);
    gainChangeTraceDb = pluginrack::makeMultibandGainChangeTrace(effect, reductionsDb, 256);
    repaint();
}

void MultibandCurveComponent::setSpectrum(const float* magnitudeDb, int numBins, double rateHz) {
    if (rateHz > 0.0) spectrumSampleRateHz = rateHz;
    if (magnitudeDb == nullptr || numBins <= 0) {
        spectrumDb.clear();
    } else {
        spectrumDb.assign(magnitudeDb, magnitudeDb + numBins);
    }
    repaint();
}

void MultibandCurveComponent::setSpectrumOffsetDb(float offsetDb) {
    if (spectrumOffsetDb == offsetDb) return;
    spectrumOffsetDb = offsetDb;
    repaint();
}

void MultibandCurveComponent::setSpectrumWeighting(SpectrumWeighting newWeighting) {
    if (spectrumWeighting == newWeighting) return;
    spectrumWeighting = newWeighting;
    repaint();
}

void MultibandCurveComponent::setGainReduction(const std::vector<float>& reductions) {
    reductionsDb = reductions;
    gainChangeTraceDb = pluginrack::makeMultibandGainChangeTrace(effect, reductionsDb, 256);
    repaint();
}

void MultibandCurveComponent::setSelectedBand(int index) {
    if (index < 0 || index >= static_cast<int>(effect.bands.size()) || index == selectedBand) return;
    selectedBand = index;
    repaint();
}

juce::Colour MultibandCurveComponent::bandColour(std::size_t index) {
    static const juce::Colour colours[] = {
        theme::cyan(), theme::green(), theme::amber(), theme::violet(),
        theme::blue(), theme::pink()
    };
    return colours[index % std::size(colours)];
}

float MultibandCurveComponent::freqToX(double hz) const {
    const auto range = std::log(kMaxFreqHz / kMinFreqHz);
    return static_cast<float>(std::log(juce::jlimit(kMinFreqHz, kMaxFreqHz, hz) / kMinFreqHz)
                              / range * getWidth());
}

double MultibandCurveComponent::xToFreq(float x) const {
    const float t = juce::jlimit(0.0f, 1.0f, x / static_cast<float>(juce::jmax(1, getWidth())));
    return kMinFreqHz * std::pow(kMaxFreqHz / kMinFreqHz, static_cast<double>(t));
}

int MultibandCurveComponent::bandAtX(float x) const {
    const double hz = xToFreq(x);
    for (std::size_t i = 0; i < effect.crossovers.size(); ++i)
        if (hz < effect.crossovers[i]) return static_cast<int>(i);
    return static_cast<int>(effect.bands.size()) - 1;
}

int MultibandCurveComponent::crossoverAt(juce::Point<float> position) const {
    for (std::size_t i = 0; i < effect.crossovers.size(); ++i) {
        const float x = freqToX(effect.crossovers[i]);
        if (std::abs(position.x - x) <= kCrossoverHitRadius) return static_cast<int>(i);
    }
    return -1;
}

float MultibandCurveComponent::dbToY(float db, juce::Rectangle<float> graph) const {
    const float t = (juce::jlimit(kMinDb, kMaxDb, db) - kMinDb) / (kMaxDb - kMinDb);
    return graph.getBottom() - t * graph.getHeight();
}

void MultibandCurveComponent::notifyEffectChanged() {
    normaliseEffect(effect);
    if (onEffectChanged) onEffectChanged(effect);
    repaint();
}

void MultibandCurveComponent::mouseDown(const juce::MouseEvent& e) {
    dragCrossover = crossoverAt(e.position);
    draggingCrossover = dragCrossover >= 0;
    if (draggingCrossover) { setMouseCursor(juce::MouseCursor::LeftRightResizeCursor); return; }
    const int band = bandAtX(e.position.x);
    if (band != selectedBand) {
        selectedBand = band;
        if (onBandSelected) onBandSelected();
        repaint();
    }
}

void MultibandCurveComponent::mouseDrag(const juce::MouseEvent& e) {
    if (!draggingCrossover || dragCrossover < 0 || dragCrossover >= static_cast<int>(effect.crossovers.size())) return;
    const float lower = dragCrossover == 0
        ? static_cast<float>(kMinFreqHz)
        : effect.crossovers[static_cast<std::size_t>(dragCrossover - 1)] + kMinBandWidthHz;
    const float upper = dragCrossover + 1 == static_cast<int>(effect.crossovers.size())
        ? static_cast<float>(kMaxFreqHz)
        : effect.crossovers[static_cast<std::size_t>(dragCrossover + 1)] - kMinBandWidthHz;
    effect.crossovers[static_cast<std::size_t>(dragCrossover)] =
        juce::jlimit(lower, upper, static_cast<float>(xToFreq(e.position.x)));
    notifyEffectChanged();
}

void MultibandCurveComponent::mouseUp(const juce::MouseEvent&) {
    draggingCrossover = false;
    dragCrossover = -1;
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void MultibandCurveComponent::mouseMove(const juce::MouseEvent& e) {
    const bool hovering = crossoverAt(e.position) >= 0;
    if (hovering == hoveringCrossover) return;
    hoveringCrossover = hovering;
    setMouseCursor(hovering ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void MultibandCurveComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    const auto graph = TraceGraphComponent::graphBounds(getLocalBounds());
    if (graph.getWidth() <= 1.0f || graph.getHeight() <= 1.0f) return;

    TraceGraphComponent::drawFrequency(
        g, bounds,
        FrequencyTraceData{targetTraceDb, spectrumDb, gainChangeTraceDb,
                           spectrumSampleRateHz, spectrumOffsetDb, spectrumWeighting});

    const auto bandLeft = [this, &graph](std::size_t index) {
        const double leftHz = index == 0 ? kMinFreqHz : effect.crossovers[index - 1];
        return graph.getX() + freqToX(leftHz) * graph.getWidth() / static_cast<float>(getWidth());
    };
    const auto bandRight = [this, &graph](std::size_t index) {
        const double rightHz = index >= effect.crossovers.size() ? kMaxFreqHz : effect.crossovers[index];
        return graph.getX() + freqToX(rightHz) * graph.getWidth() / static_cast<float>(getWidth());
    };

    for (std::size_t i = 0; i < effect.bands.size(); ++i) {
        const float left = bandLeft(i);
        const float right = bandRight(i);
        const auto colour = bandColour(i);
        g.setColour(colour.withAlpha(i == static_cast<std::size_t>(selectedBand) ? 0.16f : 0.08f));
        g.fillRect(left, graph.getY(), right - left, graph.getHeight());

        const auto& band = effect.bands[i];
        const float thresholdY = dbToY(band.thresholdDb, graph);
        g.setColour(colour.withAlpha(i == static_cast<std::size_t>(selectedBand) ? 0.8f : 0.38f));
        g.drawHorizontalLine(juce::roundToInt(thresholdY), left + 4.0f, right - 4.0f);
        if (i == static_cast<std::size_t>(selectedBand)) {
            g.setColour(colour.withAlpha(0.9f));
            g.drawRect(juce::Rectangle<float>(left + 1.0f, graph.getY() + 1.0f,
                                              right - left - 2.0f, graph.getHeight() - 2.0f), 1.0f);
        }
    }

    for (std::size_t i = 0; i < effect.crossovers.size(); ++i) {
        const float x = graph.getX() + freqToX(effect.crossovers[i])
            * graph.getWidth() / static_cast<float>(getWidth());
        g.setColour(theme::text().withAlpha(0.75f));
        g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
        g.setColour(bandColour(i).brighter(0.35f));
        g.fillEllipse(x - kCrossoverHandleRadius, graph.getY() - kCrossoverHandleRadius,
                      kCrossoverHandleRadius * 2.0f, kCrossoverHandleRadius * 2.0f);
        g.setColour(theme::bg());
        g.drawText(frequencyText(effect.crossovers[i]), juce::roundToInt(x - 28.0f),
                   juce::roundToInt(graph.getY() - 18.0f), 56, 12, juce::Justification::centred);
    }

    const float legendY = graph.getY() - 20.0f;
    g.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
    g.setColour(theme::cyan());
    g.drawText("TARGET", juce::roundToInt(graph.getX() + 4.0f),
               juce::roundToInt(legendY), 72, 12, juce::Justification::left);
    g.setColour(theme::text());
    g.drawText("RTA", juce::roundToInt(graph.getX() + 68.0f),
               juce::roundToInt(legendY), 36, 12, juce::Justification::left);
    g.setColour(theme::green());
    g.drawText("GAIN CHANGE", juce::roundToInt(graph.getX() + 100.0f),
               juce::roundToInt(legendY), 96, 12, juce::Justification::left);
}
