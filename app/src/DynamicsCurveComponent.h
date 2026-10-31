#pragma once

#include <JuceHeader.h>

#include <pluginrack/DynamicsEffect.h>

class DynamicsCurveComponent final : public juce::Component {
public:
    DynamicsCurveComponent() = default;
    void setEffect(const pluginrack::DynamicsEffect& effect);
    void setLiveLevels(float inputDb, float outputDb);
    void paint(juce::Graphics& g) override;

private:
    pluginrack::DynamicsEffect effect;
    float inputLevelDb = -60.0f;
    float outputLevelDb = -60.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DynamicsCurveComponent)
};
