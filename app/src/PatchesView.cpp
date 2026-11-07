#include "PatchesView.h"

namespace {
constexpr int kRowH = 64;
constexpr int kLabelW = 190;
constexpr int kTopPad = 54;
constexpr int kSidePad = 28;

// Dashed bezier: JUCE strokes are solid-only, so approximate the dash by
// drawing short segments along the cubic, alternating on/off.
void drawDashedCubic(juce::Graphics& g, juce::Point<float> from, juce::Point<float> to,
                     juce::Colour colour) {
    const float midX = (from.x + to.x) * 0.5f;
    const auto p = [&from, &to, midX](float t) -> juce::Point<float> {
        const float u = 1.0f - t;
        const float x = u*u*u*from.x + 3.0f*u*u*t*midX + 3.0f*u*t*t*midX + t*t*t*to.x;
        const float y = u*u*u*from.y + 3.0f*u*u*t*from.y + 3.0f*u*t*t*to.y + t*t*t*to.y;
        return {x, y};
    };
    constexpr int kSteps = 72;
    constexpr int kDashOn = 4;  // steps of curve per dash
    constexpr int kDashOff = 3;
    g.setColour(colour.withAlpha(0.85f));
    for (int s = 0; s < kSteps; s += kDashOn + kDashOff) {
        juce::Path dash;
        const int end = juce::jmin(kSteps, s + kDashOn);
        dash.startNewSubPath(p(static_cast<float>(s) / kSteps).x,
                             p(static_cast<float>(s) / kSteps).y);
        for (int i = s + 1; i <= end; ++i) {
            dash.lineTo(p(static_cast<float>(i) / kSteps).x,
                        p(static_cast<float>(i) / kSteps).y);
        }
        g.strokePath(dash, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded), {});
    }
}

} // namespace

PatchesView::PatchesView(AudioEngine& engine) : audioEngine(engine) {
    addAndMakeVisible(header);
    rebuild();
}

void PatchesView::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
    auto area = getLocalBounds().reduced(theme::ui::kPagePad);
    header.setBounds(area.removeFromTop(theme::ui::kHeaderH));
    area.removeFromTop(theme::ui::kHeaderGap);

    const int rowCount = juce::jmax(static_cast<int>(inputs.size()),
                                    static_cast<int>(outputs.size()));
    (void)area;
    if (rowCount == 0) {
        g.setColour(theme::dim());
        g.setFont(theme::ui::bodyFont());
        g.drawText("No channels or outputs yet.", getLocalBounds(),
                   juce::Justification::centred, true);
        return;
    }

    // Cables first (behind the labels), drawn as dashed cubic curves.
    // When one node is selected, only its connections are drawn.
    const int selInput = selection.isInput ? selection.index : -1;
    const int selOutput = selection.isInput ? -1 : selection.index;
    for (const auto& cable : cables) {
        if (cable.fromIndex < 0 || cable.fromIndex >= (int)inputs.size()
            || cable.toIndex < 0 || cable.toIndex >= (int)outputs.size())
            continue;
        if (selInput >= 0 && cable.fromIndex != selInput) continue;
        if (selOutput >= 0 && cable.toIndex != selOutput) continue;
        const auto from = inputs[static_cast<std::size_t>(cable.fromIndex)].port.toFloat();
        const auto to = outputs[static_cast<std::size_t>(cable.toIndex)].port.toFloat();
        drawDashedCubic(g, from, to, cable.colour);
    }

    // Input labels: hollow pills — the channel's own colour as the outline,
    // dark and hollow in the middle, channel-coloured text. The selected
    // one gets a solid fill. Socket dot outlined on the right edge.
    for (int i = 0; i < (int)inputs.size(); ++i) {
        const auto& node = inputs[static_cast<std::size_t>(i)];
        const auto accent = theme::stripColour(node.hue);
        const bool selected = selInput == i;
        g.setColour(selected ? accent : theme::panel());
        g.fillRoundedRectangle(node.box.toFloat(), 8.0f);
        g.setColour(accent);
        g.drawRoundedRectangle(node.box.toFloat().reduced(0.5f), 8.0f, selected ? 2.5f : 2.0f);
        g.setColour(selected ? theme::bg() : accent);
        g.setFont(theme::ui::bodyBoldFont());
        g.drawText(node.name.toUpperCase(), node.box.reduced(10, 0),
                   juce::Justification::centred, true);
        g.setColour(accent);
        if (selected)
            g.fillEllipse(node.port.x - 5.0f, node.port.y - 5.0f, 10.0f, 10.0f);
        else
            g.drawEllipse(node.port.x - 5.0f, node.port.y - 5.0f, 10.0f, 10.0f, 1.5f);
    }

    // Output labels: hollow pills in the output's own colour, exactly like
    // the inputs — outline in the output colour, hollow dark centre,
    // output-coloured text. Solid fill means selected.
    for (int i = 0; i < (int)outputs.size(); ++i) {
        const auto& node = outputs[static_cast<std::size_t>(i)];
        const auto accent = theme::stripColour(node.hue);
        const bool selected = selOutput == i;
        g.setColour(selected ? accent : theme::panel());
        g.fillRoundedRectangle(node.box.toFloat(), 8.0f);
        g.setColour(accent);
        g.drawRoundedRectangle(node.box.toFloat().reduced(0.5f), 8.0f, selected ? 2.5f : 2.0f);
        g.setColour(selected ? theme::bg() : accent);
        g.setFont(theme::ui::bodyBoldFont());
        g.drawText(node.name.toUpperCase(), node.box.reduced(10, 0),
                   juce::Justification::centred, true);
        g.setColour(accent);
        if (selected)
            g.fillEllipse(node.port.x - 5.0f, node.port.y - 5.0f, 10.0f, 10.0f);
        else
            g.drawEllipse(node.port.x - 5.0f, node.port.y - 5.0f, 10.0f, 10.0f, 1.5f);
    }
}

void PatchesView::mouseDown(const juce::MouseEvent& e) {
    const auto pos = e.getPosition();
    for (int i = 0; i < (int)inputs.size(); ++i) {
        if (inputs[static_cast<std::size_t>(i)].box.contains(pos)) {
            if (selection.isInput && selection.index == i)
                selection.index = -1; // clicking the selected node again clears it
            else
                selection = {true, i};
            repaint();
            return;
        }
    }
    for (int i = 0; i < (int)outputs.size(); ++i) {
        if (outputs[static_cast<std::size_t>(i)].box.contains(pos)) {
            if (!selection.isInput && selection.index == i)
                selection.index = -1;
            else
                selection = {false, i};
            repaint();
            return;
        }
    }
    // Clicking empty space clears the selection.
    if (selection.index >= 0) {
        selection.index = -1;
        repaint();
    }
}

void PatchesView::resized() {
    const int h = getHeight();
    const int rowCount = juce::jmax(static_cast<int>(inputs.size()),
                                    static_cast<int>(outputs.size()));
    const int rowsH = rowCount * kRowH;
    const int inset = kTopPad + juce::jmax(0, h - kTopPad - rowsH) / 2;

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const int y = inset + (int)i * kRowH;
        inputs[i].box = {kSidePad, y, kLabelW, 36};
        inputs[i].port = {kSidePad + kLabelW + 10, y + 18};
    }
    const int w = getWidth();
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const int y = inset + (int)i * kRowH;
        outputs[i].box = {w - kSidePad - kLabelW, y, kLabelW, 36};
        outputs[i].port = {w - kSidePad - kLabelW - 10, y + 18};
    }
    repaint();
}

void PatchesView::rebuild() {
    inputs.clear();
    outputs.clear();
    cables.clear();

    const auto& channels = audioEngine.getChannels();
    const auto& outs = audioEngine.getOutputs();
    for (const auto& ch : channels) {
        inputs.push_back({ch->name, ch->colourHue, {}, {}});
    }
    for (const auto& out : outs) {
        outputs.push_back({out->name, out->colourHue, {}, {}});
    }

    // One cable per active send (gain above the -60 dB floor), coloured by
    // the destination output's identity colour.
    for (int ci = 0; ci < (int)channels.size(); ++ci) {
        for (int oi = 0; oi < (int)outs.size(); ++oi) {
            const float gain = audioEngine.getSendGainL(channels[static_cast<std::size_t>(ci)]->id,
                                                        outs[static_cast<std::size_t>(oi)]->id);
            if (gain <= -59.0f) continue;
            cables.push_back({ci, oi, theme::stripColour(outs[static_cast<std::size_t>(oi)]->colourHue), gain});
        }
    }

    header.setStatus(juce::String(inputs.size()) + " inputs - "
                     + juce::String(outputs.size()) + " outputs - "
                     + juce::String(cables.size()) + " patches"
                     + (selection.index >= 0 ? " - showing selected" : ""));
    resized();
}

