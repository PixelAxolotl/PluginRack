#include "InsertMiniView.h"

#include <algorithm>
#include <cmath>

#include "DynamicsCurveComponent.h"
#include "MultibandCurveComponent.h"

namespace {
constexpr int kGaugeWidth = 10;
constexpr int kGaugeCaptionHeight = 10;
constexpr float kMaxReductionDb = 24.0f;
// Transfer curves are drawn over this input range, matching the range the
// dynamics panel's own axes cover.
constexpr float kMinTransferDb = -60.0f;
constexpr int kCurveSegments = 32;

// Full-width previews: every default plugin's mini graph draws edge to edge
// of the card. Axis labels live *inside* the graph rect (drawn over it), and
// the only inset is the 2px card border — no stacked margins from callers.
constexpr float kFullWidthFrame = 2.0f;
} // namespace

InsertMiniView::InsertMiniView() {
    setInterceptsMouseClicks(false, false);
}

void InsertMiniView::showEq(const std::vector<pluginrack::EqBand>& bands, double initialRateHz) {
    kind = Kind::Eq;
    if (eqCurve == nullptr) {
        eqCurve = std::make_unique<EQCurveComponent>();
        // Thumbnail only: bands are dragged in the bay's EQ panel.
        eqCurve->setInterceptsMouseClicks(false, false);
        addAndMakeVisible(*eqCurve);
    }
    eqCurve->setSampleRateHz(initialRateHz);
    eqCurve->setBands(bands);
    // The saved offset arrives on the next tick; this is the default view, so
    // the first frame already has the fill lifted off the floor.
    spectrumOffsetDb = 0.0f;
    eqCurve->setSpectrumOffsetDb(EQCurveComponent::kDefaultSpectrumOffsetDb);
    resized();
}

void InsertMiniView::showDynamics(const pluginrack::DynamicsEffect& effect) {
    kind = Kind::Dynamics;
    dynamics = effect;
    hasGate = false;
    resized();
    repaint();
}

void InsertMiniView::showDynamicsPair(const pluginrack::DynamicsEffect& compressor,
                                      const pluginrack::DynamicsEffect& gate) {
    kind = Kind::Dynamics;
    dynamics = compressor;
    gateEffect = gate;
    hasGate = true;
    resized();
    repaint();
}

void InsertMiniView::showMultiband(const pluginrack::MultibandEffect& effect, double initialRateHz) {
    kind = Kind::Multiband;
    multiband = effect;
    sampleRateHz = initialRateHz > 0.0 ? initialRateHz : 48000.0;
    spectrumOffsetDb = 0.0f;
    updateTraceVectors();
    resized();
    repaint();
}

void InsertMiniView::showDeEsser(const pluginrack::DeEsserEffect& effect, double initialRateHz) {
    kind = Kind::DeEsser;
    deEsser = effect;
    sampleRateHz = initialRateHz > 0.0 ? initialRateHz : 48000.0;
    spectrumOffsetDb = 0.0f;
    updateTraceVectors();
    resized();
    repaint();
}

void InsertMiniView::showVst() {
    kind = Kind::Vst;
    resized();
    repaint();
}

void InsertMiniView::setMultiband(const pluginrack::MultibandEffect& effect, double rateHz) {
    if (kind != Kind::Multiband) return;
    const bool changed = multiband != effect
        || !juce::approximatelyEqual(sampleRateHz, rateHz > 0.0 ? rateHz : sampleRateHz);
    if (!changed) return;
    multiband = effect;
    sampleRateHz = rateHz > 0.0 ? rateHz : sampleRateHz;
    updateTraceVectors();
    repaint();
}

void InsertMiniView::setDeEsser(const pluginrack::DeEsserEffect& effect, double rateHz) {
    if (kind != Kind::DeEsser) return;
    const bool changed = deEsser != effect
        || !juce::approximatelyEqual(sampleRateHz, rateHz > 0.0 ? rateHz : sampleRateHz);
    if (!changed) return;
    deEsser = effect;
    sampleRateHz = rateHz > 0.0 ? rateHz : sampleRateHz;
    updateTraceVectors();
    repaint();
}

void InsertMiniView::setGainReduction(const std::vector<float>& newReductionsDb) {
    if (kind != Kind::Multiband) return;
    reductionsDb = newReductionsDb;
    updateTraceVectors();
    repaint();
}

void InsertMiniView::setDeEsserGainReduction(float reductionDb) {
    if (kind != Kind::DeEsser) return;
    const float clamped = juce::jlimit(0.0f, 24.0f, reductionDb);
    if (juce::approximatelyEqual(clamped, deEsserGainReductionDb)) return;
    deEsserGainReductionDb = clamped;
    updateTraceVectors();
    repaint();
}

void InsertMiniView::updateTraceVectors() {
    if (kind == Kind::Multiband) {
        MultibandCurveComponent::normaliseEffect(multiband);
        multibandTarget = pluginrack::makeMultibandTargetTrace(multiband, 128);
        multibandGainChange = pluginrack::makeMultibandGainChangeTrace(
            multiband, reductionsDb, multibandTarget.size());
    } else if (kind == Kind::DeEsser) {
        deEsserGainChange = pluginrack::makeDeEsserGainReductionTrace(
            deEsser, deEsserGainReductionDb, 128);
    }
}

void InsertMiniView::setVstSnapshot(const juce::Image& snapshot) {
    if (kind != Kind::Vst || snapshot.isNull()) return;
    vstSnapshot = snapshot;
    repaint();
}

void InsertMiniView::setSpectrum(const float* magnitudesDb, int numBins) {
    if (kind == Kind::Eq) {
        if (eqCurve == nullptr) return;
        eqCurve->setSpectrum(magnitudesDb, numBins);
        return;
    }
    if (kind != Kind::Multiband && kind != Kind::DeEsser) return;
    if (magnitudesDb == nullptr || numBins <= 0) spectrum.clear();
    else spectrum.assign(magnitudesDb, magnitudesDb + numBins);
    repaint();
}

void InsertMiniView::setEqBands(const std::vector<pluginrack::EqBand>& bands, double initialRateHz) {
    if (eqCurve == nullptr) return;
    // Comparing first keeps a still EQ from repainting on every tick.
    if (eqCurve->getBands() == bands && juce::approximatelyEqual(eqCurve->getSampleRateHz(), initialRateHz))
        return;
    eqCurve->setSampleRateHz(initialRateHz);
    eqCurve->setBands(bands);
}

void InsertMiniView::setSpectrumOffsetDb(float storedDb) {
    const float clamped = juce::jlimit(-60.0f, 60.0f, storedDb);
    if (juce::approximatelyEqual(clamped, spectrumOffsetDb)) return;
    spectrumOffsetDb = clamped;
    if (eqCurve != nullptr && kind == Kind::Eq)
        eqCurve->setSpectrumOffsetDb(EQCurveComponent::kDefaultSpectrumOffsetDb + clamped);
    repaint();
}

void InsertMiniView::setWeighting(int index) {
    const int clamped = juce::jlimit(0, 2, index);
    if (clamped == weightingIndex) return;
    weightingIndex = clamped;
    if (eqCurve != nullptr && kind == Kind::Eq)
        eqCurve->setWeighting(EQCurveComponent::weightingFromIndex(clamped));
    repaint();
}

void InsertMiniView::setDynamics(const pluginrack::DynamicsEffect& effect) {
    if (kind != Kind::Dynamics || hasGate || dynamics == effect) return;
    dynamics = effect;
    repaint();
}

void InsertMiniView::setDynamicsPair(const pluginrack::DynamicsEffect& compressor,
                                     const pluginrack::DynamicsEffect& gate) {
    if (kind != Kind::Dynamics) return;
    if (hasGate && dynamics == compressor && gateEffect == gate) return;
    dynamics = compressor;
    gateEffect = gate;
    hasGate = true;
    repaint();
}

void InsertMiniView::setLiveGain(float newInputDb, float newGainDb) {
    const float clampedInput = juce::jlimit(kMinTransferDb, 0.0f, newInputDb);
    const float clampedGain = juce::jlimit(-kMaxReductionDb, 0.0f, newGainDb);
    hasLive = true;
    if (juce::approximatelyEqual(clampedInput, inputDb)
        && juce::approximatelyEqual(clampedGain, gainDb)) {
        return;
    }
    inputDb = clampedInput;
    gainDb = clampedGain;
    repaint();
}

void InsertMiniView::resized() {
    // EQ's embedded curve fills the whole card (was reduced(2) — fine —
    // but the *painted* grid inside it drew labels at x=7..41 and
    // edge-biased the outer frequency labels inward, so a 20Hz/20kHz
    // tick sat 20px from the edge and looked clipped). resized() now
    // gives the curve the full bounds and EQCurveComponent draws its
    // axis labels flush to the edges.
    auto area = getLocalBounds().reduced(static_cast<int>(kFullWidthFrame));
    if (eqCurve != nullptr) eqCurve->setBounds(kind == Kind::Eq ? area : juce::Rectangle<int>());
}

void InsertMiniView::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(theme::control());
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(theme::border());
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);

    switch (kind) {
        case Kind::Vst: paintVst(g); break;
        case Kind::Dynamics: paintDynamics(g); break;
        case Kind::Multiband: paintMultiband(g); break;
        case Kind::DeEsser: paintDeEsser(g); break;
        case Kind::Eq: break; // the embedded curve paints itself
        case Kind::None: break;
    }
}

void InsertMiniView::paintVst(juce::Graphics& g) {
    auto area = getLocalBounds().reduced(2);
    if (vstSnapshot.isNull()) {
        g.setColour(theme::dim());
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        g.drawText("open to capture preview", area, juce::Justification::centred, false);
        return;
    }
    // Fit by width: a wide plugin editor shrinks to the card, a tall one still
    // has to fit its height.
    g.drawImage(vstSnapshot, area.toFloat(), juce::RectanglePlacement::centred, false);
}

void InsertMiniView::paintMultiband(juce::Graphics& g) {
    const auto graph = TraceGraphComponent::miniGraphBounds(getLocalBounds());
    g.setColour(theme::bg());
    g.fillRoundedRectangle(graph, 2.0f);
    TraceGraphComponent::drawMiniTrace(g, graph, multibandTarget, theme::cyan(), true);
    TraceGraphComponent::drawSpectrum(g, graph, spectrum, sampleRateHz,
                                      kTraceRtaDefaultOffsetDb + spectrumOffsetDb,
                                      weightingFromIndex(weightingIndex));
    TraceGraphComponent::drawMiniTrace(g, graph, multibandGainChange, theme::green(), false);
    const auto freqToX = [graph](double hz) {
        const double pos = std::log(juce::jlimit(20.0, 20000.0, hz) / 20.0) / std::log(1000.0);
        return graph.getX() + static_cast<float>(pos) * graph.getWidth();
    };
    for (std::size_t i = 0; i < multiband.crossovers.size(); ++i) {
        const float x = freqToX(multiband.crossovers[i]);
        g.setColour(theme::text().withAlpha(0.75f));
        g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
    }
    g.setColour(theme::dim());
    g.setFont(juce::Font(juce::FontOptions(7.0f)));
    for (double f : {20.0, 100.0, 1000.0, 10000.0, 20000.0}) {
        const auto x = freqToX(f);
        const int labelX = juce::jlimit(juce::roundToInt(graph.getX()),
                                        juce::jmax(juce::roundToInt(graph.getX()),
                                                   juce::roundToInt(graph.getRight()) - 36),
                                        juce::roundToInt(x - 18.0f));
        g.drawText(f >= 1000.0 ? juce::String(f / 1000.0, 0) + "k" : juce::String(f, 0),
                   labelX, juce::roundToInt(graph.getBottom() + 1.0f),
                   36, 10, juce::Justification::centred);
    }
}

void InsertMiniView::paintDeEsser(juce::Graphics& g) {
    // Full width: the gauge overlays the right edge instead of stealing
    // 14px from the graph. The GR bar sits on top of the trace's right end.
    auto area = getLocalBounds().reduced(static_cast<int>(kFullWidthFrame));
    auto gauge = area.removeFromRight(kGaugeWidth);
    const auto gaugeCaption = gauge.removeFromBottom(kGaugeCaptionHeight);
    area.removeFromRight(2);
    const auto graph = TraceGraphComponent::miniGraphBounds(area);
    g.setColour(theme::bg());
    g.fillRoundedRectangle(graph, 2.0f);
    TraceGraphComponent::drawSpectrum(g, graph, spectrum, sampleRateHz,
                                      kTraceRtaDefaultOffsetDb + spectrumOffsetDb,
                                      weightingFromIndex(weightingIndex));
    TraceGraphComponent::drawMiniTrace(g, graph, deEsserGainChange, theme::red(), false);
    const float freqX = graph.getX() + static_cast<float>(
        std::log(juce::jlimit(20.0, 20000.0, static_cast<double>(deEsser.freqHz)) / 20.0)
        / std::log(1000.0)) * graph.getWidth();
    g.setColour(theme::text().withAlpha(0.8f));
    g.drawVerticalLine(juce::roundToInt(freqX), graph.getY(), graph.getBottom());
    const float thresholdY = TraceGraphComponent::miniTraceDb(deEsser.thresholdDb, graph);
    g.setColour(theme::cyan().withAlpha(0.65f));
    g.drawHorizontalLine(juce::roundToInt(thresholdY), graph.getX(), graph.getRight());
    g.setColour(theme::bg().darker(0.3f));
    g.fillRoundedRectangle(gauge.toFloat(), 2.0f);
    const float fill = juce::jlimit(0.0f, 1.0f, deEsserGainReductionDb / kMaxReductionDb);
    if (fill > 0.0f) {
        auto filled = gauge.toFloat();
        filled.setHeight(filled.getHeight() * fill);
        g.setColour(theme::red());
        g.fillRoundedRectangle(filled, 2.0f);
    }
    g.setColour(theme::dim());
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    g.drawText("GR", gaugeCaption, juce::Justification::centred, false);
}

void InsertMiniView::paintDynamics(juce::Graphics& g) {
    // Full width: same overlay-gauge treatment as De-esser — the transfer
    // curve runs to the card's right edge, gauge floats over it.
    auto area = getLocalBounds().reduced(static_cast<int>(kFullWidthFrame));
    auto gauge = area.removeFromRight(kGaugeWidth);
    const auto gaugeCaption = gauge.removeFromBottom(kGaugeCaptionHeight);
    area.removeFromRight(2);

    const float w = static_cast<float>(area.getWidth());
    const float h = static_cast<float>(area.getHeight());
    if (w < 8.0f || h < 8.0f) return;

    const float xAt = static_cast<float>(area.getX());
    const float yAt = static_cast<float>(area.getY());
    const auto toX = [&](float db) {
        return xAt + (db - kMinTransferDb) / -kMinTransferDb * w;
    };
    const auto toY = [&](float db) {
        return yAt + h - (db - kMinTransferDb) / -kMinTransferDb * h;
    };
    const auto gainDbFor = [](const pluginrack::DynamicsEffect& effect, float db) {
        if (effect.bypassed) return 0.0f;
        return effect.type == pluginrack::DynamicsEffectType::Gate
                   ? DynamicsCurveComponent::gateGainDb(db, effect)
                   : DynamicsCurveComponent::staticGainDb(db, effect);
    };
    const auto strokeResponse = [&](const pluginrack::DynamicsEffect& effect,
                                    juce::Colour colour, float thickness) {
        juce::Path response;
        for (int i = 0; i <= kCurveSegments; ++i) {
            const float db = kMinTransferDb
                             + (0.0f - kMinTransferDb) * static_cast<float>(i)
                                   / static_cast<float>(kCurveSegments);
            const auto point = juce::Point<float>(
                toX(db), toY(juce::jlimit(kMinTransferDb, 0.0f, db + gainDbFor(effect, db))));
            if (i == 0) response.startNewSubPath(point);
            else response.lineTo(point);
        }
        g.setColour(colour);
        g.strokePath(response, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    };

    g.setColour(theme::border().withAlpha(0.5f));
    g.drawLine(toX(kMinTransferDb), toY(kMinTransferDb), toX(0.0f), toY(0.0f), 1.0f);

    // A bypassed curve is dimmed rather than hidden, so the card never
    // disagrees with its switches.
    if (hasGate) {
        // Only the part of the gate that deviates from unity, closed by the
        // step that lifts the muted level back to the knee.
        const float gateEndDb = juce::jlimit(
            kMinTransferDb, 0.0f,
            gateEffect.thresholdDb + std::max(gateEffect.kneeDb, 0.0f) * 0.5f);
        juce::Path gatePath;
        gatePath.startNewSubPath(
            toX(kMinTransferDb),
            toY(juce::jlimit(kMinTransferDb, 0.0f,
                             kMinTransferDb + gainDbFor(gateEffect, kMinTransferDb))));
        for (int i = 1; i <= kCurveSegments; ++i) {
            const float db = kMinTransferDb
                             + (gateEndDb - kMinTransferDb) * static_cast<float>(i)
                                   / static_cast<float>(kCurveSegments);
            gatePath.lineTo(
                toX(db), toY(juce::jlimit(kMinTransferDb, 0.0f, db + gainDbFor(gateEffect, db))));
        }
        gatePath.lineTo(toX(gateEndDb), toY(gateEndDb));
        g.setColour(gateEffect.bypassed ? theme::dim().withAlpha(0.45f) : theme::cyan());
        g.strokePath(gatePath, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }
    strokeResponse(dynamics, dynamics.bypassed ? theme::dim() : theme::amber(), 1.6f);

    if (hasLive) {
        g.setColour(theme::text());
        const auto dot = juce::Rectangle<float>(4.0f, 4.0f)
                             .withCentre({ toX(inputDb), toY(inputDb + gainDb) });
        g.fillEllipse(dot);
    }

    g.setColour(theme::bg().darker(0.3f));
    g.fillRoundedRectangle(gauge.toFloat(), 2.0f);
    const float fill = juce::jlimit(0.0f, 1.0f, -gainDb / kMaxReductionDb);
    if (fill > 0.0f) {
        auto filled = gauge.toFloat();
        filled.setHeight(filled.getHeight() * fill);
        g.setColour(theme::amber());
        g.fillRoundedRectangle(filled, 2.0f);
    }
    g.setColour(theme::dim());
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    g.drawText("GR", gaugeCaption, juce::Justification::centred, false);
}

MiniSpectrum::MiniSpectrum()
    : fft(std::make_unique<juce::dsp::FFT>(13)), // 2^13 == kFftSize
      hann(static_cast<std::size_t>(kFftSize)),
      scratch(static_cast<std::size_t>(kFftSize) * 2, 0.0f),
      fresh(static_cast<std::size_t>(kFftSize) / 2 + 1, 0.0f),
      smoothed(static_cast<std::size_t>(kFftSize) / 2 + 1, -100.0f) {
    for (int i = 0; i < kFftSize; ++i) {
        const double t = juce::MathConstants<double>::twoPi * static_cast<double>(i)
                         / static_cast<double>(kFftSize);
        hann[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(t));
    }
}

void MiniSpectrum::refresh(const float* left, const float* right) {
    for (int i = 0; i < kFftSize; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        scratch[idx] = (left[idx] + right[idx]) * 0.5f * hann[idx];
    }
    fft->performFrequencyOnlyForwardTransform(scratch.data(), true);

    constexpr float norm = 4.0f / static_cast<float>(kFftSize);
    for (std::size_t bin = 0; bin < fresh.size(); ++bin) {
        const float mag = scratch[bin] * norm;
        fresh[bin] = juce::jmax(-100.0f, 20.0f * std::log10(juce::jmax(1e-5f, mag)));
        float& held = smoothed[bin];
        held += (fresh[bin] - held) * (fresh[bin] > held ? 0.6f : 0.15f);
    }
}

void DynamicsMeterSim::update(const float* left, const float* right, int frames,
                              const pluginrack::DynamicsEffect& effect, double timerHz) {
    float peak = 0.0f;
    for (int i = 0; i < frames; ++i)
        peak = std::max(peak, std::max(std::abs(left[i]), std::abs(right[i])));

    inputDb = juce::jlimit(-60.0f, 0.0f, 20.0f * std::log10(std::max(peak, 1.0e-6f)));
    const float dotInputDb = juce::jlimit(-60.0f, 0.0f,
                                         inputDb + (effect.bypassed ? 0.0f : effect.inputGainDb));
    const float targetDb =
        effect.bypassed ? 0.0f
                        : (effect.type == pluginrack::DynamicsEffectType::Gate
                               ? DynamicsCurveComponent::gateGainDb(dotInputDb, effect)
                               : DynamicsCurveComponent::staticGainDb(dotInputDb, effect));

    // Same UI-rate detector ballistics as the dynamics panel.
    const auto coefficient = [timerHz](float milliseconds) {
        return 1.0f - std::exp(-1.0f
                               / (static_cast<float>(timerHz) * std::max(milliseconds, 0.1f) * 0.001f));
    };
    const float rate = targetDb < gainDb ? coefficient(effect.attackMs)
                                         : coefficient(effect.releaseMs);
    gainDb += (targetDb - gainDb) * rate;
}
