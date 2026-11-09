#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "MixerComponent.h"
#include "PageHeader.h"
#include "Theme.h"

// The I/O page: the same strips the Mixer shows, split so inputs and outputs
// each get their own tab. Device assignment stays on the strips, so this page
// and the Mixer can never disagree. Round-trip measurement lives in Settings.
class IoView final : public juce::Component, private juce::Timer {
public:
    explicit IoView(AudioEngine& engine);
    ~IoView() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

    // Strips hold references to engine state, so callers rebuild after any
    // structural change (add/remove/restore) and before the strips die.
    void rebuild();
    void refresh();
    // Device assignments changed in the engine (a background open finished);
    // update the strips in place instead of rebuilding them.
    void refreshDeviceStrips();

    // A strip added, removed or renamed here changes every other view.
    std::function<void()> onStructureChanged;
    // Forwarded from both boards so double-click in I/O jumps to the Rack
    // like it does in the Mixer.
    std::function<void(int channelId, bool isOutput)> onSelectionChanged;
    std::function<void(int channelId, bool isOutput)> onDoubleClicked;

private:
    void timerCallback() override;
    void selectTab(bool showInputTab);

    AudioEngine& audioEngine;

    PageHeader header{ "Input & Output" };
    juce::TextButton inputsTab{ "Inputs" };
    juce::TextButton outputsTab{ "Outputs" };
    MixerComponent inputsBoard;
    MixerComponent outputsBoard;

    bool showInputs = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IoView)
};
