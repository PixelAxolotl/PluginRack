#pragma once

#include <JuceHeader.h>

#include <pluginrack/DynamicsEffect.h>

class DynamicsCurveComponent final : public juce::Component {
public:
    DynamicsCurveComponent() = default;
    void setEffect(const pluginrack::DynamicsEffect& effect);
    // Gate curve drawn alongside the compressor's; `visible` follows the gate's
    // bypass so a bypassed gate does not show a curve it is not applying.
    void setGate(const pluginrack::DynamicsEffect& gate, bool visible);
    // Live dot: the level the compressor sees and the level it puts out.
    void setLivePoint(float inputDb, float outputDb);
    // Steady-state gain in dB for an input level (threshold/ratio/knee only).
    static float staticGainDb(float inputDb, const pluginrack::DynamicsEffect& effect);
    static float gateGainDb(float inputDb, const pluginrack::DynamicsEffect& gate);
    void paint(juce::Graphics& g) override;

private:
    pluginrack::DynamicsEffect effect;
    pluginrack::DynamicsEffect gate;
    bool gateVisible = false;
    float liveInputDb = -60.0f;
    float liveOutputDb = -60.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DynamicsCurveComponent)
};
