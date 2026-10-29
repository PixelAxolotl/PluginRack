#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

#include <pluginrack/EqEngine.h>

// Pro-Q-style curve editor (Task 5) for the fixed six-band EQ. Paints a
// log-frequency grid (20 Hz - 20 kHz, +/-15 dB), one draggable node per band
// in its band colour, and the combined magnitude response in yellow computed
// from the same RBJ biquad transfer functions core/EqEngine.cpp applies
// (formula duplicated here because EqEngine::computeCoeffs is private and
// core is frozen; the active-band rule mirrors EqEngine::rebuild).
// The component owns the band state; EQWindow pushes edits through setBands
// and persists them through onBandsChanged.
class EQCurveComponent final : public juce::Component {
public:
    static constexpr double kMinFreqHz = 20.0;      // X axis range (UI binding)
    static constexpr double kMaxFreqHz = 20000.0;
    static constexpr double kMaxGainDb = 15.0;      // Y axis range is +/-15 dB
    static constexpr double kMinQ = 0.1;            // matches SceneValidator
    static constexpr double kMaxQ = 20.0;

    EQCurveComponent();

    void setBands(const std::vector<pluginrack::EqBand>& newBands);
    const std::vector<pluginrack::EqBand>& getBands() const { return bands; }

    int getSelectedBand() const { return selectedBand; }
    void setSelectedBand(int index);

    // Fires after any user edit (node drag) so EQWindow can push the bands
    // to the audio engine and refresh the owning strip's EQ button.
    std::function<void()> onBandsChanged;
    // Fires when a click picks a different node: selection alone is not a
    // band change, but the window must re-sync its knobs to the new band.
    std::function<void()> onBandSelected;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override { dragBand = -1; }

private:
    static juce::Colour bandColour(int index);
    static juce::Point<float> nodePosition(const pluginrack::EqBand& band,
                                           int width, int height);
    // Normalised biquad magnitude in dB at hz, mirroring core's coefficient
    // maths. sampleRateHz is the UI's reference rate (device rate at build).
    static double bandShapeDb(const pluginrack::EqBand& band,
                              double sampleRateHz, double hz);

    double xToFreq(float x) const;
    float freqToX(double hz) const;
    double yToGain(float y) const;
    float gainToY(double gainDb) const;

    std::vector<pluginrack::EqBand> bands{pluginrack::defaultSixBands()};
    int selectedBand = 2; // first peak band: a meaningful default selection
    int dragBand = -1;    // band being dragged this gesture, -1 when idle
    // Curve reference rate. The app engine runs at 48 kHz by default and the
    // normalised magnitude response is visually indistinguishable at 44.1;
    // re-querying the engine per repaint would need a lock. UI-thread only.
    double sampleRateHz = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EQCurveComponent)
};
