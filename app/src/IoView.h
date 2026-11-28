#pragma once

#include <JuceHeader.h>

#include <functional>

#include "AudioEngine.h"
#include "MixerComponent.h"
#include "PageHeader.h"
#include "Theme.h"

// The I/O page: the same strips the Mixer shows, split so inputs, outputs and
// buses each get their own tab. Device assignment stays on the strips, so this
// page and the Mixer can never disagree. Round-trip measurement lives in Settings.
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
    enum class Tab { Inputs, Outputs, Buses };

    void timerCallback() override;
    void selectTab(Tab tab);
    // Rebuilds the channel-pair picker from the selected output: items 1/2,
    // 3/4, ... up to the open device's width, or a fixed 1/2..7/8 list while
    // the device is unknown. Disabled with no selection, no device, or a bus.
    void updatePairCombo();

    AudioEngine& audioEngine;

    PageHeader header{ "Input & Output" };
    juce::TextButton inputsTab{ "Inputs" };
    juce::TextButton outputsTab{ "Outputs" };
    juce::TextButton busesTab{ "Buses" };
    // Compact device channel-pair picker for the selected output strip. The
    // strips themselves belong to MixerComponent, so the picker lives in the
    // tab row instead of on the strip.
    juce::Label pairLabel{ {}, "From:" };
    juce::ComboBox pairCombo;
    std::uint32_t selectedOutputId = 0;
    MixerComponent inputsBoard;
    MixerComponent outputsBoard;
    MixerComponent busesBoard;

    Tab currentTab = Tab::Inputs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IoView)
};
