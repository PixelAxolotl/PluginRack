#pragma once

#include <JuceHeader.h>

#include <pluginrack/DynamicsEffect.h>

class DynamicsCurveComponent final : public juce::Component {
public:
    DynamicsCurveComponent() = default;
    void setEffect(const pluginrack::DynamicsEffect& effect);
    // Gate curve drawn alongside the compressor's; `active` follows the gate's
    // bypass and dims the curve rather than hiding it, so the gate's shape and
    // its knobs never disagree.
    void setGate(const pluginrack::DynamicsEffect& gate, bool active);
    // Live dot: the level the compressor sees and the level it puts out.
    void setLivePoint(float inputDb, float outputDb);
    // Steady-state gain in dB for an input level (threshold/ratio/knee only).
    static float staticGainDb(float inputDb, const pluginrack::DynamicsEffect& effect);
    static float gateGainDb(float inputDb, const pluginrack::DynamicsEffect& gate);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    // Static curves (identity, compressor transfer, gate transfer) are built
    // once per effect/gate/size change: paint() runs at the live-meter rate and
    // must not re-walk the gain law 120 times per frame.
    void rebuildCurves(juce::Rectangle<float> graph);

    pluginrack::DynamicsEffect effect;
    pluginrack::DynamicsEffect gate;
    bool gatePresent = false;
    bool gateActive = false;
    float liveInputDb = -60.0f;
    float liveOutputDb = -60.0f;
    juce::Path identityPath;
    juce::Path transferPath;
    juce::Path gatePath;
    bool curvesDirty = true;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DynamicsCurveComponent)
};
