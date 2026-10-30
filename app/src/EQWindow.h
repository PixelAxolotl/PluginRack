#pragma once

#include <JuceHeader.h>

#include <juce_dsp/juce_dsp.h>

#include <functional>
#include <memory>
#include <vector>

#include <pluginrack/EqEngine.h>

#include "EQCurveComponent.h"
#include "MiniButton.h"
#include "Theme.h"

// Task-4 shell button: stock TextButton face with a flag-driven DESIGN.md 1px
// accent-cyan ring (MiniButton/DeviceControl convention). The ring paints on
// top of the face so an amber-active face keeps precedence while open.
class EqButton final : public juce::TextButton {
public:
    explicit EqButton(const juce::String& text) : juce::TextButton(text) {}

    // Right-click hook: strips use it to open the editor while left-click
    // toggles the EQ on/off. Right presses are swallowed here so they never
    // enter the button-press state (which would also fire onClick on release).
    std::function<void()> onRightClick;

    void mouseDown(const juce::MouseEvent& e) override {
        if (e.mods.isRightButtonDown() && onRightClick) {
            onRightClick();
            return;
        }
        juce::TextButton::mouseDown(e);
    }

    void setRingVisible(bool visible) {
        if (ringVisible == visible) return;
        ringVisible = visible;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        juce::TextButton::paint(g);
        if (ringVisible) {
            g.setColour(theme::cyan());
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.5f), 3.0f, 1.0f);
        }
    }

private:
    bool ringVisible = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqButton)
};

// Floating Pro-Q-style EQ editor (Task 4 shell, Task 5 editor, Task 6
// analyzer). Single instance per strip: the owner strip creates it in
// openEqWindow() and destroys it in closeEqWindow(). Close-button handling
// mirrors PluginHost's PluginWindow: the delete is deferred through
// SafePointer + callAsync so the button can finish dispatching first, and the
// owner may be gone by the time it arrives.
//
// The window owns no engine reference: initialBands is a snapshot taken at
// open time (same semantics as the title), and every edit funnels through
// applyBands, which the owner wires to setChannelEq/setOutputEq plus
// refreshEqButton so the strip's amber state tracks in-window changes.
// sampleRateHz is the owner's live device rate at open, injected into the
// curve's magnitude maths so the drawn response matches the engine's.
//
// The analyzer (Task 6) runs on a UI-thread timer: it pulls the newest
// kAnalyzerFftSize frames through the owner-wired pullAnalyzerTap (a copy of
// the engine's preallocated tap ring - no audio-thread math), Hann-weights
// the mono mixdown, runs the window's one FFT instance, and hands the bin
// magnitudes to the curve's grey fill. Off stops the timer and clears the
// fill. If sustained ticks outrun the 60 fps budget the timer drops to 30
// fps once (spec: overload slows the analyzer, never the audio).
class EQWindow final : public juce::DocumentWindow, private juce::Timer {
public:
    static constexpr int kWidth = 560;
    static constexpr int kHeight = 440;

    // Analyzer mode codes: the setAnalyzerTapMode callback forwards these to
    // AudioEngine::AnalyzerTap::setMode (kOff/kPre/kPost mirror).
    static constexpr int kAnalyzerOff = 0;
    static constexpr int kAnalyzerPre = 1;
    static constexpr int kAnalyzerPost = 2;
    static constexpr int kAnalyzerFftSize = 8192; // == AnalyzerTap::kSamples

    EQWindow(const juce::String& title,
             const std::vector<pluginrack::EqBand>& initialBands,
             std::function<void(const std::vector<pluginrack::EqBand>&)> applyBands,
             double sampleRateHz);
    ~EQWindow() override;

    // Owner wiring (assign after construction, same pattern as onCloseRequest):
    // pull copies the newest kAnalyzerFftSize frames of the tapped side
    // (postEq selects the ring render() wrote around the EQ call) into
    // destL/destR of that capacity; setAnalyzerTapMode gates the ring write.
    std::function<void(bool postEq, float* destL, float* destR)> pullAnalyzerTap;
    std::function<void(int mode)> setAnalyzerTapMode;

    // Switches between kAnalyzerOff/Pre/Post; button clicks route through it.
    void setAnalyzerMode(int mode);

    void closeButtonPressed() override;
    void resized() override;

    std::function<void()> onCloseRequest;

private:
    void timerCallback() override;
    void applyFromControls(); // knob/button edits -> curve repaint + apply
    void syncControls();      // reflect the selected band into knobs/buttons

    EQCurveComponent curve;
    std::function<void(const std::vector<pluginrack::EqBand>&)> applyBands;

    MiniButton autoButton{"Auto"};        // visual only in v1 (plan, Task 5)
    MiniButton bypassButton{"Bypass"};    // selected band enabled toggle
    MiniButton soloButton{"Solo"};        // selected band solo toggle
    juce::Label stereoTag{"", "Stereo"};  // visual only in v1 (plan, Task 5)
    juce::Label freqLabel{"", "Freq"};
    juce::Label gainLabel{"", "Gain"};
    juce::Label qLabel{"", "Q"};
    juce::Slider freqKnob;
    juce::Slider gainKnob;
    juce::Slider qKnob;

    // Analyzer state, all sized once at construction (message thread): one
    // FFT instance per open window (spec), reused every tick.
    MiniButton rtaMinusButton{"-"};
    MiniButton rtaPlusButton{"+"};
    juce::Label analyzerLabel{"", "Analyzer"};
    juce::Label rtaOffsetLabel{"", "+12"};
    static constexpr float kRtaOffsetMinDb = -60.0f;
    static constexpr float kRtaOffsetMaxDb = 60.0f;
    static constexpr float kRtaOffsetStepDb = 3.0f;
    static constexpr float kDefaultRtaOffsetDb = 24.0f;
    void setRtaOffsetDb(float offsetDb);
    int analyzerMode = kAnalyzerOff;
    bool reducedToFps30 = false;
    int slowTicks = 0;
    std::unique_ptr<juce::dsp::FFT> analyzerFft;
    std::vector<float> hannCoeffs;  // cyclic Hann over the ring window
    std::vector<float> fftScratch;  // 2N floats: N real inputs + engine workspace
    std::vector<float> tapL;
    std::vector<float> tapR;
    std::vector<float> spectrumDb;  // FFT bins 0..N/2, dBFS, fed to the curve
    // Smoothed copy of spectrumDb (fast attack, slow release per bin) so
    // music/noise reads as a steady Pro-Q-style trace instead of flicker.
    // Same size as spectrumDb, UI thread only, allocated at construction.
    std::vector<float> spectrumAvg;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EQWindow)
};
