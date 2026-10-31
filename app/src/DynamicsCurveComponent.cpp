#include "DynamicsCurveComponent.h"

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {
constexpr float kMinDb = -60.0f;
constexpr float kMaxDb = 0.0f;
constexpr int kGridSteps = 5;

float proportionForDb(float db) {
    return (juce::jlimit(kMinDb, kMaxDb, db) - kMinDb) / (kMaxDb - kMinDb);
}
}

// Same static gain law DynamicsProcessor applies: threshold, ratio and knee
// only, so input trim and output gain never bend the drawn curve.
float DynamicsCurveComponent::staticGainDb(float inputDb,
                                           const pluginrack::DynamicsEffect& effect) {
    // Bypassed draws the straight line it actually applies.
    if (effect.bypassed) return 0.0f;
    if (effect.ratio <= 1.0f) return 0.0f;
    const float distance = inputDb - effect.thresholdDb;
    const float knee = std::max(effect.kneeDb, 0.0f);
    if (knee > 0.0f && distance > -knee * 0.5f && distance < knee * 0.5f) {
        const float width = distance + knee * 0.5f;
        return (1.0f / effect.ratio - 1.0f) * width * width / (2.0f * knee);
    }
    if (distance > 0.0f) return distance * (1.0f / effect.ratio - 1.0f);
    return 0.0f;
}

void DynamicsCurveComponent::setEffect(const pluginrack::DynamicsEffect& newEffect) {
    effect = newEffect;
    repaint();
}

void DynamicsCurveComponent::setGate(const pluginrack::DynamicsEffect& newGate, bool visible) {
    gate = newGate;
    gateVisible = visible;
    repaint();
}

// Mirrors the processor's expander law: Range dB of attenuation below the
// threshold, spread over the knee.
float DynamicsCurveComponent::gateGainDb(float inputDb,
                                         const pluginrack::DynamicsEffect& gate) {
    const float range = std::max(gate.ratio, 0.0f);
    const float knee = std::max(gate.kneeDb, 0.0f);
    const float distance = inputDb - gate.thresholdDb;
    if (knee > 0.0f && distance < knee * 0.5f && distance > -knee * 0.5f)
        return -range * (knee * 0.5f - distance) / knee;
    if (distance <= -knee * 0.5f) return -range;
    return 0.0f;
}

void DynamicsCurveComponent::setLivePoint(float inputDb, float outputDb) {
    liveInputDb = inputDb;
    liveOutputDb = outputDb;
    repaint();
}

void DynamicsCurveComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(theme::control());
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(theme::border());
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

    auto inner = bounds.reduced(10.0f);
    const auto caption = inner.removeFromTop(12.0f);
    inner.removeFromLeft(26.0f);   // output scale
    inner.removeFromRight(30.0f);  // output meter + labels
    inner.removeFromBottom(26.0f); // input meter + labels
    const float side = std::max(1.0f, std::min(inner.getWidth(), inner.getHeight()));
    const auto graph = inner.withSizeKeepingCentre(side, side);

    const auto toPoint = [&graph](float inputDb, float outputDbValue) {
        return juce::Point<float>(
            graph.getX() + graph.getWidth() * (inputDb - kMinDb) / (kMaxDb - kMinDb),
            graph.getBottom() - graph.getHeight() *
                (juce::jlimit(kMinDb, kMaxDb, outputDbValue) - kMinDb) / (kMaxDb - kMinDb));
    };

    g.setColour(theme::line().withAlpha(0.5f));
    for (int i = 0; i <= kGridSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kGridSteps);
        g.drawVerticalLine(juce::roundToInt(graph.getX() + graph.getWidth() * t),
                           graph.getY(), graph.getBottom());
        g.drawHorizontalLine(juce::roundToInt(graph.getY() + graph.getHeight() * t),
                             graph.getX(), graph.getRight());
    }

    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    for (int i = 0; i <= kGridSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kGridSteps);
        const int db = juce::roundToInt(kMinDb + (kMaxDb - kMinDb) * t);
        const int x = juce::roundToInt(graph.getX() + graph.getWidth() * t);
        const int y = juce::roundToInt(graph.getBottom() - graph.getHeight() * t);
        g.setColour(theme::dim());
        g.drawText(juce::String(db), x - 16, juce::roundToInt(graph.getBottom()) + 11, 32, 10,
                   juce::Justification::centred);
        g.drawText(juce::String(db), juce::roundToInt(graph.getX()) - 26, y - 5, 23, 10,
                   juce::Justification::right);
    }

    juce::Path identity;
    juce::Path transfer;
    identity.startNewSubPath(toPoint(kMinDb, kMinDb));
    transfer.startNewSubPath(toPoint(kMinDb, kMinDb + staticGainDb(kMinDb, effect)));
    for (int i = 1; i <= 120; ++i) {
        const float input = kMinDb + (kMaxDb - kMinDb) * static_cast<float>(i) / 120.0f;
        identity.lineTo(toPoint(input, input));
        transfer.lineTo(toPoint(input, input + staticGainDb(input, effect)));
    }
    g.setColour(theme::dim().withAlpha(0.45f));
    g.strokePath(identity, juce::PathStrokeType(1.0f));

    if (gateVisible) {
        // Only the part that deviates from unity: past the threshold the gate
        // is the identity line the compressor curve is already drawn over.
        const float gateEndDb = juce::jlimit(
            kMinDb, kMaxDb, gate.thresholdDb + std::max(gate.kneeDb, 0.0f) * 0.5f);
        juce::Path gatePath;
        gatePath.startNewSubPath(toPoint(kMinDb, kMinDb + gateGainDb(kMinDb, gate)));
        for (int i = 1; i <= 120; ++i) {
            const float input = kMinDb + (gateEndDb - kMinDb) * static_cast<float>(i) / 120.0f;
            gatePath.lineTo(toPoint(input, input + gateGainDb(input, gate)));
        }
        // Close the step back onto unity: without this the vertical edge that
        // lifts the muted level to the threshold is missing.
        gatePath.lineTo(toPoint(gateEndDb, gateEndDb));
        g.setColour(theme::green());
        g.strokePath(gatePath, juce::PathStrokeType(2.0f));
    }

    g.setColour(theme::cyan());
    g.strokePath(transfer, juce::PathStrokeType(2.0f));

    const auto dot = toPoint(liveInputDb, liveOutputDb);
    g.setColour(theme::blue().withAlpha(0.22f));
    g.fillEllipse(dot.x - 8.0f, dot.y - 8.0f, 16.0f, 16.0f);
    g.setColour(theme::blue());
    g.fillEllipse(dot.x - 4.5f, dot.y - 4.5f, 9.0f, 9.0f);

    // Level meters hang off the graph edges: input along the bottom, output
    // up the right side, so the dot reads against both axes.
    const auto meter = [&g](juce::Rectangle<float> bar, float level, bool horizontal) {
        g.setColour(theme::panel());
        g.fillRoundedRectangle(bar, 2.0f);
        g.setColour(theme::line().withAlpha(0.45f));
        g.drawRoundedRectangle(bar, 2.0f, 1.0f);
        const float filled = proportionForDb(level);
        if (filled <= 0.0f) return;
        auto lit = bar;
        if (horizontal) lit.setWidth(std::max(1.0f, bar.getWidth() * filled));
        else lit.setTop(bar.getBottom() - std::max(1.0f, bar.getHeight() * filled));
        juce::ColourGradient gradient(
            theme::green(), horizontal ? bar.getX() : bar.getCentreX(),
            horizontal ? bar.getCentreY() : bar.getBottom(),
            theme::amber(), horizontal ? bar.getRight() : bar.getCentreX(),
            horizontal ? bar.getCentreY() : bar.getY(), false);
        g.setGradientFill(gradient);
        g.fillRoundedRectangle(lit, 2.0f);
    };
    meter({ graph.getX(), graph.getBottom() + 3.0f, graph.getWidth(), 6.0f }, liveInputDb, true);
    meter({ graph.getRight() + 3.0f, graph.getY(), 6.0f, graph.getHeight() }, liveOutputDb, false);

    g.setColour(theme::dim());
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    g.drawText("out dB",
               juce::Rectangle<int>(juce::roundToInt(graph.getX()), juce::roundToInt(caption.getY()),
                                    60, 12),
               juce::Justification::centredLeft);
    g.drawText("in dB",
               juce::Rectangle<int>(juce::roundToInt(graph.getRight()) - 60,
                                    juce::roundToInt(caption.getY()), 60, 12),
               juce::Justification::centredRight);
}
