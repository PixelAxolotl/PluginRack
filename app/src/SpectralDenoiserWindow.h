#pragma once

#include <JuceHeader.h>
#include <pluginrack/SpectralDenoiserEffect.h>
#include "Theme.h"
#include "ValueEditSlider.h"

class AudioEngine;

// Editor for the built-in spectral denoiser. Learn captures one second of
// noise-only audio through the live engine; the finished profile is stored in
// the effect, so it saves with the scene like any other setting.
class SpectralDenoiserWindow final : public juce::DocumentWindow, private juce::Timer {
public:
    using ApplyCallback = std::function<void(const pluginrack::SpectralDenoiserEffect&, bool bypassChanged)>;

    static constexpr int kWidth = theme::ui::kEffectWindowW;
    static constexpr int kHeight = 380;

    SpectralDenoiserWindow(const juce::String& title,
                           const pluginrack::SpectralDenoiserEffect& initialEffect,
                           AudioEngine& engine, bool isOutput, std::uint32_t stripId,
                           ApplyCallback apply);
    ~SpectralDenoiserWindow() override = default;

    void resized() override;
    void timerCallback() override;
    void setBypassed(bool b);
    void closeButtonPressed() override { if (onClose) onClose(); }

    std::function<void()> onClose;

private:
    void startLearning();
    void clearProfile();
    void refreshStatus();

    pluginrack::SpectralDenoiserEffect effect;
    ApplyCallback apply;
    AudioEngine& audioEngine;
    const bool outputStrip;
    const std::uint32_t targetId;

    juce::Component content;
    juce::Label modeLabel{"", "SPECTRAL DENOISER"};
    juce::TextButton enableButton{"Enable"};
    juce::ComboBox modeCombo;
    ValueEditSlider reduction;
    juce::Label reductionLabel{"", "Max Reduction"};
    juce::TextButton learnButton{"Learn"};
    juce::TextButton clearButton{"Clear Profile"};
    juce::Label statusLabel;

    bool learning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralDenoiserWindow)
};
