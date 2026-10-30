#pragma once

#include <JuceHeader.h>

#include <functional>
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

// Floating Pro-Q-style EQ editor (Task 4 shell, Task 5 editor). Single
// instance per strip: the owner strip creates it in openEqWindow() and
// destroys it in closeEqWindow(). Close-button handling mirrors PluginHost's
// PluginWindow: the delete is deferred through SafePointer + callAsync so the
// button can finish dispatching first, and the owner may be gone by the time
// it arrives.
//
// The window owns no engine reference: initialBands is a snapshot taken at
// open time (same semantics as the title), and every edit funnels through
// applyBands, which the owner wires to setChannelEq/setOutputEq plus
// refreshEqButton so the strip's amber state tracks in-window changes.
// sampleRateHz is the owner's live device rate at open, injected into the
// curve's magnitude maths so the drawn response matches the engine's.
class EQWindow final : public juce::DocumentWindow {
public:
    static constexpr int kWidth = 560;
    static constexpr int kHeight = 440;

    EQWindow(const juce::String& title,
             const std::vector<pluginrack::EqBand>& initialBands,
             std::function<void(const std::vector<pluginrack::EqBand>&)> applyBands,
             double sampleRateHz);

    void closeButtonPressed() override;
    void resized() override;

    std::function<void()> onCloseRequest;

private:
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EQWindow)
};
