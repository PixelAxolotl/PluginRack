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

    // Reference rate for the biquad magnitude maths. EQWindow injects the
    // engine's live device rate at open; 48000 is only the pre-injection
    // fallback. Non-positive rates are ignored.
    void setSampleRateHz(double rateHz);
    double getSampleRateHz() const { return sampleRateHz; }

    int getSelectedBand() const { return selectedBand; }
    void setSelectedBand(int index);

    // Combined magnitude in dB that the yellow curve would show at hz, given
    // the current bands and the reference rate: exactly the maths paint()
    // draws, exposed for verification (sine-vs-curve checks).
    double magnitudeDbAt(double hz) const;

    // Live analyzer spectrum behind the curve (Task 6). magnitudeDb holds one
    // FFT-bin magnitude in dBFS, bin i covering i * nyquist / (numBins - 1)
    // Hz at the reference rate; nullptr/0 clears it (analyzer off). UI thread
    // only, like the rest of the component.
    void setSpectrum(const float* magnitudeDb, int numBins);

    // RTA display offset in dB, added to every analyzer bin before it is
    // mapped to Y (UI thread only). Lets the user lift quiet program
    // material into the +/-15 dB view or push hot signals down; the EQ
    // curves themselves are unaffected.
    void setSpectrumOffsetDb(float offsetDb);
    float getSpectrumOffsetDb() const { return spectrumOffsetDb; }

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
    // Y of the analyzer fill at hz: linear interpolation between FFT-bin
    // magnitudes, clamped to the +/-15 dB view (0 when no spectrum is set).
    float spectrumY(double hz) const;

    std::vector<pluginrack::EqBand> bands{pluginrack::defaultSixBands()};
    int selectedBand = 2; // first peak band: a meaningful default selection
    int dragBand = -1;    // band being dragged this gesture, -1 when idle
    // Analyzer magnitudes in dBFS per FFT bin (empty = off). UI thread only,
    // written by EQWindow's timer through setSpectrum, read by paint().
    std::vector<float> spectrumDb;
    // Curve reference rate: the live device rate injected by EQWindow at
    // open (setSampleRateHz). 48000 is the pre-injection fallback matching
    // the engine default; non-positive rates never land here. UI-thread only.
    double sampleRateHz = 48000.0;
    // Added to each analyzer bin in spectrumY before the view clamp, so the
    // RTA fill can be shifted up/down without touching the EQ response.
    // Session-only (resets per window); the engine never sees it.
    float spectrumOffsetDb = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EQCurveComponent)
};
