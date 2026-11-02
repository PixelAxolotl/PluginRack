#include "InsertMiniView.h"

#include <algorithm>
#include <cmath>

#include "DynamicsCurveComponent.h"

namespace {
constexpr int kGaugeWidth = 10;
constexpr int kGaugeCaptionHeight = 10;
constexpr float kMaxReductionDb = 24.0f;
// Transfer curves are drawn over this input range, matching the range the
// dynamics panel's own axes cover.
constexpr float kMinTransferDb = -60.0f;
constexpr int kCurveSegments = 32;
} // namespace

InsertMiniView::InsertMiniView() {
    setInterceptsMouseClicks(false, false);
}

void InsertMiniView::showEq(const std::vector<pluginrack::EqBand>& bands, double sampleRateHz) {
    kind = Kind::Eq;
    if (eqCurve == nullptr) {
        eqCurve = std::make_unique<EQCurveComponent>();
        // Thumbnail only: bands are dragged in the bay's EQ panel.
        eqCurve->setInterceptsMouseClicks(false, false);
        addAndMakeVisible(*eqCurve);
    }
    eqCurve->setSampleRateHz(sampleRateHz);
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

void InsertMiniView::showVst() {
    kind = Kind::Vst;
    resized();
    repaint();
}

void InsertMiniView::setVstSnapshot(const juce::Image& snapshot) {
    if (kind != Kind::Vst || snapshot.isNull()) return;
    vstSnapshot = snapshot;
    repaint();
}

void InsertMiniView::setSpectrum(const float* magnitudesDb, int numBins) {
    if (eqCurve == nullptr) return;
    eqCurve->setSpectrum(magnitudesDb, numBins);
}

void InsertMiniView::setEqBands(const std::vector<pluginrack::EqBand>& bands, double sampleRateHz) {
    if (eqCurve == nullptr) return;
    // Comparing first keeps a still EQ from repainting on every tick.
    if (eqCurve->getBands() == bands && juce::approximatelyEqual(eqCurve->getSampleRateHz(), sampleRateHz))
        return;
    eqCurve->setSampleRateHz(sampleRateHz);
    eqCurve->setBands(bands);
}

void InsertMiniView::setSpectrumOffsetDb(float storedDb) {
    if (eqCurve == nullptr) return;
    const float clamped = juce::jlimit(-60.0f, 60.0f, storedDb);
    if (juce::approximatelyEqual(clamped, spectrumOffsetDb)) return;
    spectrumOffsetDb = clamped;
    eqCurve->setSpectrumOffsetDb(EQCurveComponent::kDefaultSpectrumOffsetDb + clamped);
}

void InsertMiniView::setWeighting(int index) {
    if (eqCurve == nullptr) return;
    const int clamped = juce::jlimit(0, 2, index);
    if (clamped == weightingIndex) return;
    weightingIndex = clamped;
    eqCurve->setWeighting(EQCurveComponent::weightingFromIndex(clamped));
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
    auto area = getLocalBounds().reduced(2);
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

void InsertMiniView::paintDynamics(juce::Graphics& g) {
    auto area = getLocalBounds().reduced(4);
    auto gauge = area.removeFromRight(kGaugeWidth);
    const auto gaugeCaption = gauge.removeFromBottom(kGaugeCaptionHeight);
    area.removeFromRight(4);

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

    // Both effects of the pair share these axes. A bypassed curve is dimmed
    // rather than hidden, so the card never disagrees with its switches.
    if (hasGate) {
        // Straight from the editor's own draw: only the part of the gate that
        // deviates from unity, closed by the step that lifts the muted level to
        // the knee. Drawing the whole range would just retrace the identity
        // line past the threshold, which is what the panel avoids.
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
