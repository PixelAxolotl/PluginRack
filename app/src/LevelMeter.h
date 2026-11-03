#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>

// Stereo level meter for a strip's fader column. The solid bar is RMS, the
// tinted block above it is peak, and the held peak is a thin line that resets
// every interval. Bars start and end on the fader's travel, so a height reads
// against the same dB on the fader beside it; the space above the travel holds
// the Peak/RMS readouts.
class LevelMeter final : public juce::Component,
                         public juce::SettableTooltipClient,
                         private juce::Timer {
public:
    LevelMeter(juce::Slider& faderToFollow,
               juce::Atomic<float>& leftPeakSource,
               juce::Atomic<float>& rightPeakSource,
               juce::Atomic<float>& leftRmsSource,
               juce::Atomic<float>& rightRmsSource);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void setStereo(bool stereo);

    static constexpr float kMinDb = -60.0f;
    static constexpr float kMaxDb = 12.0f;

    // Width the strip layouts reserve for a meter: two bars, the dB scale and
    // the Peak/RMS readouts all fit in it.
    static constexpr int kPreferredWidth = 44;

    // Peak-hold intervals offered in the meter's right-click menu.
    inline static constexpr std::array<int, 5> kPeakHoldChoices{100, 250, 500, 750, 1000};

    // Shared by every meter; the menu sets it for the session.
    static int peakHoldMs();
    static void setPeakHoldMs(int ms);

private:
    void timerCallback() override;

    // The vertical extent a fader's value travels through, in the fader's
    // parent coordinates. A slider maps its value across this whole region;
    // the track it paints is inset from it, so the meter has to use the
    // region, not the painted track, to line up with the handle.
    static juce::Rectangle<int> faderValueBounds(juce::Slider& fader);

    juce::Slider& fader;
    juce::Atomic<float>& leftPeakSource;
    juce::Atomic<float>& rightPeakSource;
    juce::Atomic<float>& leftRmsSource;
    juce::Atomic<float>& rightRmsSource;

    float displayedPeakL = 0.0f;
    float displayedPeakR = 0.0f;
    float displayedRmsL = 0.0f;
    float displayedRmsR = 0.0f;
    float heldPeakL = 0.0f;
    float heldPeakR = 0.0f;
    juce::uint32 holdStartL = 0;
    juce::uint32 holdStartR = 0;
    bool stereo = true;

    inline static std::atomic<int> peakHoldMsValue{500};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeter)
};
