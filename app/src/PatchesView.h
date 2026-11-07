#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "PageHeader.h"
#include "Theme.h"

// Patches tab: inputs down the left, outputs down the right, and a dashed
// curved cable for every active send (gain above -60 dB). Cables are
// coloured by the destination output's identity colour. Inputs follow
// their channel colour (hollow outline pills); outputs are solid chips.
// Clicking a node selects it and isolates its connections. Read-only in
// v1: patching happens in the Routing matrix; this view is the map.
class PatchesView final : public juce::Component {
public:
    explicit PatchesView(AudioEngine& engine);

    void resized() override;
    void paint(juce::Graphics& g) override;
    void rebuild();
    void mouseDown(const juce::MouseEvent& e) override;

private:
    // Selection state. -1 = nothing selected.
    struct Selection {
        bool isInput = true;
        int index = -1;
    };

    struct Node {
        juce::String name;
        float hue = 0.0f;
        juce::Point<int> port; // socket centre in local coords
        juce::Rectangle<int> box;
    };

    AudioEngine& audioEngine;
    PageHeader header{"Patches"};
    std::vector<Node> inputs;
    std::vector<Node> outputs;
    struct Cable {
        int fromIndex = 0;
        int toIndex = 0;
        juce::Colour colour;
        float gainDb = -60.0f;
    };
    std::vector<Cable> cables;
    Selection selection;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PatchesView)
};
