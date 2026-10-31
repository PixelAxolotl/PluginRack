#include "DynamicsCurveComponent.h"

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {
constexpr float kMinDb = -60.0f;
constexpr float kMaxDb = 0.0f;

float outputDb(float inputDb, const pluginrack::DynamicsEffect& effect) {
    const float input = inputDb + effect.inputGainDb;
    if (input <= effect.thresholdDb || effect.ratio <= 1.0f)
        return input + effect.makeupDb;
    return effect.thresholdDb +
           (input - effect.thresholdDb) / effect.ratio + effect.makeupDb;
}
}

void DynamicsCurveComponent::setEffect(const pluginrack::DynamicsEffect& newEffect) {
    effect = newEffect;
    repaint();
}

void DynamicsCurveComponent::setLiveLevels(float inputDb, float outputDb) {
    inputLevelDb = inputDb;
    outputLevelDb = outputDb;
    repaint();
}

void DynamicsCurveComponent::paint(juce::Graphics& g) {
    auto area = getLocalBounds().toFloat().reduced(8.0f);
    g.setColour(theme::control());
    g.fillRoundedRectangle(area, 4.0f);
    g.setColour(theme::border());
    g.drawRoundedRectangle(area, 4.0f, 1.0f);

    const auto graph = area.reduced(24.0f, 16.0f);
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    for (int db = -60; db <= 0; db += 10) {
        const float x = graph.getX() + graph.getWidth() * (db - kMinDb) / (kMaxDb - kMinDb);
        const float y = graph.getBottom() - graph.getHeight() * (db - kMinDb) / (kMaxDb - kMinDb);
        g.setColour(theme::line().withAlpha(0.65f));
        g.drawVerticalLine(static_cast<int>(x), graph.getY(), graph.getBottom());
        g.drawHorizontalLine(static_cast<int>(y), graph.getX(), graph.getRight());
        g.setColour(theme::dim());
        g.drawText(juce::String(db), static_cast<int>(x - 14.0f),
                   static_cast<int>(graph.getBottom() + 3.0f), 28, 12,
                   juce::Justification::centred);
        if (db < 0) {
            g.drawText(juce::String(db), static_cast<int>(graph.getX() - 24.0f),
                       static_cast<int>(y - 6.0f), 22, 12,
                       juce::Justification::right);
        }
    }

    auto toPoint = [&graph](float inputDb, float outputDbValue) {
        return juce::Point<float>(
            graph.getX() + graph.getWidth() * (inputDb - kMinDb) / (kMaxDb - kMinDb),
            graph.getBottom() - graph.getHeight() *
                (juce::jlimit(kMinDb, kMaxDb, outputDbValue) - kMinDb) / (kMaxDb - kMinDb));
    };

    juce::Path identity;
    juce::Path transfer;
    identity.startNewSubPath(toPoint(kMinDb, kMinDb));
    transfer.startNewSubPath(toPoint(kMinDb, outputDb(kMinDb, effect)));
    for (int i = 1; i <= 120; ++i) {
        const float input = kMinDb + (kMaxDb - kMinDb) * i / 120.0f;
        identity.lineTo(toPoint(input, input));
        transfer.lineTo(toPoint(input, outputDb(input, effect)));
    }
    g.setColour(theme::dim().withAlpha(0.7f));
    g.strokePath(identity, juce::PathStrokeType(1.0f));
    g.setColour(theme::cyan());
    g.strokePath(transfer, juce::PathStrokeType(2.0f));

    const auto inputPoint = toPoint(inputLevelDb, inputLevelDb);
    const auto outputPoint = toPoint(inputLevelDb, outputLevelDb);
    g.setColour(theme::text());
    g.drawLine(inputPoint.x, graph.getBottom(), inputPoint.x, inputPoint.y, 2.0f);
    g.setColour(theme::green());
    g.drawLine(outputPoint.x, graph.getBottom(), outputPoint.x, outputPoint.y, 2.0f);
    g.fillEllipse(inputPoint.x - 3.0f, inputPoint.y - 3.0f, 6.0f, 6.0f);
    g.fillEllipse(outputPoint.x - 3.0f, outputPoint.y - 3.0f, 6.0f, 6.0f);
    g.setColour(theme::text());
    g.drawText("IN", static_cast<int>(graph.getRight()) - 58,
               static_cast<int>(graph.getY()) + 4, 20, 12,
               juce::Justification::centred);
    g.setColour(theme::green());
    g.drawText("OUT", static_cast<int>(graph.getRight()) - 34,
               static_cast<int>(graph.getY()) + 4, 28, 12,
               juce::Justification::centred);

    const auto thresholdPoint = toPoint(effect.thresholdDb, outputDb(effect.thresholdDb, effect));
    g.setColour(theme::amber());
    g.drawVerticalLine(static_cast<int>(thresholdPoint.x), graph.getY(), graph.getBottom());
    g.fillEllipse(thresholdPoint.x - 4.0f, thresholdPoint.y - 4.0f, 8.0f, 8.0f);
    g.setColour(theme::text());
    g.drawText("INPUT dB", static_cast<int>(graph.getCentreX()) - 35, getHeight() - 16, 70, 12,
               juce::Justification::centred);
    g.drawText("OUTPUT dB", 2, static_cast<int>(graph.getY()) - 14, 70, 12,
               juce::Justification::left);
}
