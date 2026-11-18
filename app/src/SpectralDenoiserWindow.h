#pragma once

#include <JuceHeader.h>
#include <pluginrack/SpectralDenoiserEffect.h>
#include "Theme.h"
#include "ValueEditSlider.h"

#include <array>

class AudioEngine;

// Draws the learned noise profile and lets the user shape it with one trim per
// log-spaced band. The spectrum uses the profile's own sample rate so it stays
// correct after the scene is reopened at a different rate. A meter strip below
// the curve shows the live gain reduction per band.
class SpectralDenoiserProfileView final : public juce::Component {
public:
    explicit SpectralDenoiserProfileView(pluginrack::SpectralDenoiserEffect& effectToEdit);

    std::function<void()> onTrimEdited;

    void setReduction(const std::array<float, pluginrack::kSpectralDenoiserTrimBandCount>& bands);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    void ensureTrims();
    int bandAt(int x) const;
    juce::Rectangle<float> plotArea() const;
    juce::Rectangle<float> meterArea() const;
    float frequencyForBin(std::size_t bin) const;
    float frequencyForBandCentre(int band) const;
    float xForFrequency(float hz) const;
    float trimPixelsPerDb() const;

    pluginrack::SpectralDenoiserEffect& effect;
    std::array<float, pluginrack::kSpectralDenoiserTrimBandCount> reductionDb{};
    int dragBand = -1;
    float dragStartTrim = 0.0f;
    int dragStartY = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralDenoiserProfileView)
};

// Editor for the built-in spectral denoiser. Learn captures one second of
// noise-only audio through the live engine; the finished profile is stored in
// the effect, so it saves with the scene like any other setting.
class SpectralDenoiserWindow final : public juce::DocumentWindow, private juce::Timer {
public:
    using ApplyCallback = std::function<void(const pluginrack::SpectralDenoiserEffect&, bool bypassChanged)>;

    static constexpr int kWidth = theme::ui::kEffectWindowW;
    static constexpr int kHeight = 600;

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
    juce::TextButton resetTrimButton{"Reset Trim"};
    juce::Label statusLabel;
    SpectralDenoiserProfileView profileView;

    bool learning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralDenoiserWindow)
};
