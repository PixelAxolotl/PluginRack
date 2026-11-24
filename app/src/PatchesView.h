#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "PageHeader.h"
#include "Theme.h"

// Patches tab: inputs down the left, buses in the middle, outputs down the
// right, and a dashed curved cable for every active send (gain above -60 dB).
// An input expands into a card listing every tap point (Input, each chain
// item, Output); dragging from a socket onto a bus or output creates that
// connection at its tap. Right-clicking a cable removes it.
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
        enum class Kind { Input, Bus, Output };
        Kind kind = Kind::Input;
        int index = -1;
    };

    struct Socket {
        pluginrack::Tap tap;
        juce::Point<int> pos;
        juce::String label;
    };

    struct Node {
        std::uint32_t id = 0;
        juce::String name;
        float hue = 0.0f;
        juce::Point<int> port;   // right-edge socket
        juce::Point<int> portIn; // left-edge socket (buses only)
        juce::Rectangle<int> box;
        std::vector<Socket> sockets;
        bool expanded = false;
    };

    // Scrollable canvas: the header stays fixed while rows scroll.
    class PatchCanvas final : public juce::Component {
    public:
        explicit PatchCanvas(PatchesView& ownerIn) : owner(ownerIn) {}
        void paint(juce::Graphics& g) override { owner.paintCanvas(g); }
        void mouseDown(const juce::MouseEvent& e) override { owner.canvasMouseDown(e); }
        void mouseDrag(const juce::MouseEvent& e) override { owner.canvasMouseDrag(e); }
        void mouseUp(const juce::MouseEvent& e) override { owner.canvasMouseUp(e); }

    private:
        PatchesView& owner;
    };

    struct Cable {
        enum class Kind { InputToOutput, InputToBus, BusToOutput };
        Kind kind = Kind::InputToOutput;
        int from = 0;
        int to = 0;
        pluginrack::Tap tap;
        juce::Colour colour;
    };

    void paintCanvas(juce::Graphics& g);
    void canvasMouseDown(const juce::MouseEvent& e);
    void canvasMouseDrag(const juce::MouseEvent& e);
    void canvasMouseUp(const juce::MouseEvent& e);
    void paintInputCard(juce::Graphics& g, const Node& node, bool selected);
    juce::Point<int> sourcePoint(const Cable& cable) const;
    juce::Point<int> socketPoint(const Node& node, const pluginrack::Tap& tap) const;
    // Vertical control point for a cable, bowed below any bus pill the straight
    // line would cross (skipBus = the bus the cable is connecting to).
    // Downward shift of the cable's two control handles so the curve clears
    // any bus pill it would otherwise cross (skipBus = the bus it connects to).
    float cableBow(juce::Point<float> from, juce::Point<float> to,
                   int skipBus) const;
    int channelLatency(const AudioEngine::Channel& channel) const;
    int tapLatency(const AudioEngine::Channel& channel, const pluginrack::Tap& tap) const;
    void deleteCable(const Cable& cable);
    void updateStatusText();
    void updateLayout();

    AudioEngine& audioEngine;
    PageHeader header{"Patches"};
    juce::Viewport canvasViewport;
    PatchCanvas canvasContent{*this};
    std::vector<Node> inputs;
    std::vector<Node> buses;
    std::vector<Node> outputs;
    std::vector<Cable> cables;
    Selection selection;
    int expandedInput = -1;
    bool dragging = false;
    bool dragFromBus = false;
    int dragIndex = -1;
    int dragSocket = -1;
    juce::Point<int> dragPos;
    // Where each bus was dropped (canvas y of its top edge). Patches is a
    // canvas, so a bus stays where it is put.
    std::map<std::uint32_t, int> busY;
    bool draggingBus = false;
    int dragBusIndex = -1;
    int dragBusGrabOffset = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PatchesView)
};
