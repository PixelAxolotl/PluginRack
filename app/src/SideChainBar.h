#pragma once

#include <JuceHeader.h>

#include <cstdint>

#include "AudioEngine.h"

// Compact strip above the docked plugin editor: chooses the insert's
// side-chain source (a channel, an output or a dedicated aux bus), the point
// in that strip the signal is tapped from, and its trim. The bar edits the
// focused insert in place; the engine owns the routing.
class SideChainBar final : public juce::Component, private juce::Timer {
public:
    explicit SideChainBar(AudioEngine& engine);

    // The focused insert. ownerId is the channel or output that holds it.
    void setTarget(AudioEngine::InsertSlot* insert, bool isOutput, std::uint32_t ownerId);
    // Re-reads the engine's lists and the insert's values. Call after strips or
    // buses change.
    void refresh();

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

    static constexpr int kHeight = 40;

private:
    pluginrack::SideChainSource selectedSource() const;
    void rebuildSourceIds();
    void refreshValues();
    void apply();
    void addBus();
    void editBusSources();

    AudioEngine& audioEngine;
    AudioEngine::InsertSlot* target = nullptr;
    bool targetIsOutput = false;
    std::uint32_t ownerId = 0;
    // Guards the onChange callbacks while the bar writes values back.
    bool updating = false;

    juce::Label caption;
    juce::ComboBox sourceCombo;
    juce::ComboBox sourceIdCombo;
    juce::ComboBox tapCombo;
    juce::Slider gainSlider;
    juce::TextButton addBusButton{"+ Bus"};
    juce::TextButton sourcesButton{"Sources"};
    juce::Label keyLevel;
};
