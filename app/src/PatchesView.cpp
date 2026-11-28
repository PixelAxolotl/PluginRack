#include "PatchesView.h"

#include <algorithm>
#include <map>

namespace {
constexpr int kRowH = 64;
constexpr int kLabelW = 190;
// Canvas inset from the viewport edge: the same 24 px the page views use
// between control groups, so the graph starts where the other pages' content
// starts instead of floating in its own margin.
constexpr int kSidePad = theme::ui::kSectionGap;
constexpr int kPillH = 36;
constexpr int kPointH = 30;
constexpr int kCardPad = theme::ui::kRowGap;
constexpr int kCardHeaderH = 30;

// Dashed bezier: build the curve once, then let JUCE cut it along its arc
// length so every dash is the same size regardless of curve length.
void drawDashedCubic(juce::Graphics& g, juce::Point<float> from, juce::Point<float> to,
                     juce::Colour colour, float bow) {
    const float midX = (from.x + to.x) * 0.5f;
    juce::Path curve;
    curve.startNewSubPath(from);
    // Standard S-curve: horizontal handles at each end. `bow` shifts both
    // handles down together to clear a bus pill without flattening the curve.
    curve.cubicTo(midX, from.y + bow, midX, to.y + bow, to.x, to.y);

    static constexpr float kDashes[] = { 9.0f, 6.0f };
    juce::Path dashed;
    juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                         juce::PathStrokeType::rounded)
        .createDashedStroke(dashed, curve, kDashes, 2);

    g.setColour(colour.withAlpha(0.85f));
    g.fillPath(dashed);
}

float distanceToCubic(juce::Point<float> from, juce::Point<float> to,
                      juce::Point<float> point, float bow) {
    const float midX = (from.x + to.x) * 0.5f;
    float best = 1.0e9f;
    for (int i = 0; i <= 48; ++i) {
        const float t = static_cast<float>(i) / 48.0f;
        const float u = 1.0f - t;
        const float x = u*u*u*from.x + 3.0f*u*u*t*midX + 3.0f*u*t*t*midX + t*t*t*to.x;
        const float y = u*u*u*from.y + 3.0f*u*u*t*(from.y + bow)
                        + 3.0f*u*t*t*(to.y + bow) + t*t*t*to.y;
        best = juce::jmin(best, juce::Point<float>(x, y).getDistanceFrom(point));
    }
    return best;
}

juce::String chainItemLabel(const AudioEngine::Channel& channel,
                            const pluginrack::ChainItem& item) {
    switch (item.type) {
    case pluginrack::ChainItemType::Insert:
        for (const auto& insert : channel.inserts) {
            if (insert->id == item.id) {
                return insert->pluginName.isNotEmpty() ? insert->pluginName : "VST";
            }
        }
        return "VST";
    case pluginrack::ChainItemType::Eq: return "EQ";
    case pluginrack::ChainItemType::Multiband: return "Multiband Comp";
    case pluginrack::ChainItemType::DeEsser: return "De-esser";
    case pluginrack::ChainItemType::Limiter: return "Limiter";
    case pluginrack::ChainItemType::FeedbackSuppressor: return "Feedback Suppressor";
    case pluginrack::ChainItemType::SpectralDenoiser: return "Spectral Denoiser";
    case pluginrack::ChainItemType::Dynamics: {
        for (const auto& effect : channel.effects) {
            if (effect.id == item.id) {
                return effect.type == pluginrack::DynamicsEffectType::Compressor
                           ? "Compressor" : "Gate";
            }
        }
        return "Dynamics";
    }
    }
    return "Item";
}

constexpr int kNoMatch = -1;
} // namespace

PatchesView::PatchesView(AudioEngine& engine) : audioEngine(engine) {
    addAndMakeVisible(header);
    canvasViewport.setViewedComponent(&canvasContent, false);
    canvasViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(canvasViewport);
    rebuild();
}

void PatchesView::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
}

void PatchesView::paintCanvas(juce::Graphics& g) {
    g.fillAll(theme::bg());

    if (inputs.empty() && outputs.empty()) {
        g.setColour(theme::dim());
        g.setFont(theme::ui::bodyFont());
        g.drawText("No channels or outputs yet.", canvasContent.getLocalBounds(),
                   juce::Justification::centred, true);
        return;
    }

    // Cables first (behind the labels), drawn as dashed cubic curves.
    // When one node is selected, only its connections are drawn.
    const int sel = selectedIndex();
    const auto cableVisible = [this, sel](const Cable& cable) {
        if (sel < 0) return true;
        const bool input = selection.kind == Selection::Kind::Input
                           && cable.kind != Cable::Kind::BusToOutput
                           && cable.from == sel;
        const bool bus = selection.kind == Selection::Kind::Bus
                         && ((cable.kind == Cable::Kind::InputToBus && cable.to == sel)
                             || (cable.kind == Cable::Kind::BusToOutput
                                 && cable.from == sel));
        const bool output = selection.kind == Selection::Kind::Output
                            && cable.kind != Cable::Kind::InputToBus
                            && cable.to == sel;
        return input || bus || output;
    };
    for (const auto& cable : cables) {
        if (!cableVisible(cable)) continue;
        juce::Point<float> to;
        if (cable.kind == Cable::Kind::InputToBus) {
            if (cable.to < 0 || cable.to >= static_cast<int>(buses.size())) continue;
            to = buses[static_cast<std::size_t>(cable.to)].portIn.toFloat();
        } else if (cable.kind == Cable::Kind::BusToOutput) {
            if (cable.to < 0 || cable.to >= static_cast<int>(outputs.size())) continue;
            to = outputs[static_cast<std::size_t>(cable.to)].port.toFloat();
        } else {
            if (cable.to < 0 || cable.to >= static_cast<int>(outputs.size())) continue;
            to = outputs[static_cast<std::size_t>(cable.to)].port.toFloat();
        }
        drawDashedCubic(g, sourcePoint(cable).toFloat(), to, cable.colour,
                        cableBow(sourcePoint(cable).toFloat(), to,
                                      cable.kind == Cable::Kind::InputToBus ? cable.to : -1));
    }

    // Pills for buses and outputs; inputs either a pill or an expanded card.
    const auto paintPill = [&g](const Node& node, bool selected) {
        const auto accent = theme::stripColour(node.hue);
        g.setColour(selected ? accent : theme::panel());
        g.fillRoundedRectangle(node.box.toFloat(), 8.0f);
        g.setColour(accent);
        g.drawRoundedRectangle(node.box.toFloat().reduced(0.5f), 8.0f, selected ? 2.5f : 2.0f);
        g.setColour(selected ? theme::bg() : accent);
        g.setFont(theme::ui::bodyBoldFont());
        g.drawText(node.name.toUpperCase(), node.box.reduced(10, 0),
                   juce::Justification::centred, true);
        g.setColour(accent);
        const auto paintSocket = [&g, selected](juce::Point<int> p) {
            if (selected)
                g.fillEllipse(p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f);
            else
                g.drawEllipse(p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f, 1.5f);
        };
        paintSocket(node.port);
        if (node.portIn.x != 0) paintSocket(node.portIn);
    };
    const auto paintPills = [&paintPill](const auto& nodes, int selectedIndex) {
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
            paintPill(nodes[static_cast<std::size_t>(i)], selectedIndex == i);
    };

    for (int i = 0; i < static_cast<int>(inputs.size()); ++i) {
        const auto& node = inputs[static_cast<std::size_t>(i)];
        if (node.expanded) {
            paintInputCard(g, node,
                           selection.kind == Selection::Kind::Input && sel == i);
        } else {
            const auto accent = theme::stripColour(node.hue);
            const bool selected = selection.kind == Selection::Kind::Input && sel == i;
            g.setColour(selected ? accent : theme::panel());
            g.fillRoundedRectangle(node.box.toFloat(), 8.0f);
            g.setColour(accent);
            g.drawRoundedRectangle(node.box.toFloat().reduced(0.5f), 8.0f, selected ? 2.5f : 2.0f);
            g.setColour(selected ? theme::bg() : accent);
            g.setFont(theme::ui::bodyBoldFont());
            g.drawText(node.name.toUpperCase(),
                       node.box.reduced(10, 0).withTrimmedRight(20),
                       juce::Justification::centred, true);
            // Down chevron: the pill opens into its tap card.
            const int cx = node.box.getRight() - 15;
            const int cy = node.box.getCentreY();
            juce::Path chevron;
            chevron.startNewSubPath(static_cast<float>(cx - 5), static_cast<float>(cy - 3));
            chevron.lineTo(static_cast<float>(cx), static_cast<float>(cy + 3));
            chevron.lineTo(static_cast<float>(cx + 5), static_cast<float>(cy - 3));
            g.strokePath(chevron, juce::PathStrokeType(1.8f));
            g.setColour(accent);
            if (selected)
                g.fillEllipse(node.port.x - 5.0f, node.port.y - 5.0f, 10.0f, 10.0f);
            else
                g.drawEllipse(node.port.x - 5.0f, node.port.y - 5.0f, 10.0f, 10.0f, 1.5f);
        }
    }
    paintPills(buses, selection.kind == Selection::Kind::Bus ? sel : -1);
    paintPills(outputs, selection.kind == Selection::Kind::Output ? sel : -1);

    if (dragging) {
        juce::Point<int> from;
        if (dragFromBus) {
            if (dragIndex >= 0 && dragIndex < static_cast<int>(buses.size()))
                from = buses[static_cast<std::size_t>(dragIndex)].port;
            else
                return;
        } else {
            if (dragIndex < 0 || dragIndex >= static_cast<int>(inputs.size())) return;
            const auto& node = inputs[static_cast<std::size_t>(dragIndex)];
            from = dragSocket >= 0 && dragSocket < static_cast<int>(node.sockets.size())
                       ? node.sockets[static_cast<std::size_t>(dragSocket)].pos
                       : node.port;
        }
        const auto previewColour = theme::stripColour(200.0f);
        drawDashedCubic(g, from.toFloat(), dragPos.toFloat(), previewColour,
                        cableBow(from.toFloat(), dragPos.toFloat(), -1));

        // Compatible drop targets: buses and outputs for an input socket,
        // outputs only for a bus.
        g.setColour(theme::text().withAlpha(0.7f));
        const auto highlight = [&g](juce::Point<int> p) {
            g.drawEllipse(static_cast<float>(p.x) - 7.5f, static_cast<float>(p.y) - 7.5f,
                          15.0f, 15.0f, 1.5f);
        };
        for (const auto& bus : buses) highlight(bus.portIn);
        if (!dragFromBus)
            for (const auto& output : outputs) highlight(output.port);
    }
}

void PatchesView::paintInputCard(juce::Graphics& g, const Node& node, bool selected) {
    const auto accent = theme::stripColour(node.hue);
    g.setColour(selected ? accent.withAlpha(0.18f) : theme::panel());
    g.fillRoundedRectangle(node.box.toFloat(), 10.0f);
    g.setColour(accent);
    g.drawRoundedRectangle(node.box.toFloat().reduced(0.5f), 10.0f, 2.0f);

    g.setFont(theme::ui::bodyBoldFont());
    g.setColour(accent);
    g.drawText(node.name.toUpperCase(),
               node.box.withTrimmedLeft(kCardPad).withHeight(kCardHeaderH),
               juce::Justification::centredLeft, true);

    // Collapse chevron, top-right.
    const int cx = node.box.getRight() - 18;
    const int cy = node.box.getY() + kCardHeaderH / 2;
    juce::Path chevron;
    chevron.startNewSubPath(static_cast<float>(cx - 5), static_cast<float>(cy - 3));
    chevron.lineTo(static_cast<float>(cx), static_cast<float>(cy + 3));
    chevron.lineTo(static_cast<float>(cx + 5), static_cast<float>(cy - 3));
    g.strokePath(chevron, juce::PathStrokeType(1.8f));

    const auto& channel = audioEngine.getChannels();
    const double engineRate = audioEngine.getSampleRate();
    const double sampleRate = engineRate > 0.0 ? engineRate : 48000.0;

    // The badge is the cumulative latency at this socket, not the row's own
    // contribution: Input reads 0, the row that adds a delay shows the new
    // total in the strip colour, and Output reads the strip total.
    int previousLatency = 0;
    g.setFont(theme::ui::captionFont());
    for (std::size_t i = 0; i < node.sockets.size(); ++i) {
        const auto& socket = node.sockets[i];
        const int rowY = node.box.getY() + kCardHeaderH + kCardPad
                         + static_cast<int>(i) * kPointH;
        g.setColour(theme::dim());
        g.setFont(theme::ui::captionFont());
        g.drawText(socket.label, node.box.withTrimmedLeft(kCardPad)
                                     .withY(rowY).withHeight(kPointH),
                   juce::Justification::centredLeft, true);

        int latency = 0;
        for (const auto& ch : channel) {
            if (ch->id == node.id) {
                latency = audioEngine.stripTapLatencySamples(*ch, socket.tap);
                break;
            }
        }
        const bool addsLatency = latency > previousLatency;
        previousLatency = latency;
        const double ms = 1000.0 * static_cast<double>(latency) / sampleRate;
        const juce::String text =
            latency == 0 ? juce::String("0 ms") : juce::String(ms, 1) + " ms";
        g.setColour(addsLatency ? accent : theme::dim());
        g.setFont(theme::ui::numberFont(11.0f));
        g.drawText(text,
                   node.box.getRight() - 78, rowY,
                   64, kPointH, juce::Justification::centredRight, false);

        g.setColour(accent);
        g.drawEllipse(socket.pos.x - 5.0f, socket.pos.y - 5.0f, 10.0f, 10.0f, 1.5f);
    }
}

juce::Point<int> PatchesView::socketPoint(const Node& node,
                                          const pluginrack::Tap& tap) const {
    // A tap names a chain row by (id, type); a legacy id-only tap falls back to
    // the first socket carrying the id, the row the engine resolves it to.
    for (const auto& socket : node.sockets) {
        if (socket.tap.kind != tap.kind) continue;
        if (tap.kind != pluginrack::Tap::Kind::Item) return socket.pos;
        if (socket.tap.itemId != tap.itemId) continue;
        if (tap.hasItemType && socket.tap.itemType != tap.itemType) continue;
        return socket.pos;
    }
    return node.port;
}

juce::Point<int> PatchesView::sourcePoint(const Cable& cable) const {
    if (cable.kind == Cable::Kind::BusToOutput) {
        if (cable.from >= 0 && cable.from < static_cast<int>(buses.size()))
            return buses[static_cast<std::size_t>(cable.from)].port;
        return {};
    }
    if (cable.from < 0 || cable.from >= static_cast<int>(inputs.size())) return {};
    const auto& node = inputs[static_cast<std::size_t>(cable.from)];
    if (node.expanded) return socketPoint(node, cable.tap);
    return node.port;
}

float PatchesView::cableBow(juce::Point<float> from, juce::Point<float> to,
                            int skipBus) const {
    const float midX = (from.x + to.x) * 0.5f;
    const auto crosses = [&](float bow) {
        for (int i = 0; i <= 64; ++i) {
            const float t = static_cast<float>(i) / 64.0f;
            const float u = 1.0f - t;
            const float x = u*u*u*from.x + 3.0f*u*u*t*midX + 3.0f*u*t*t*midX
                            + t*t*t*to.x;
            const float y = u*u*u*from.y + 3.0f*u*u*t*(from.y + bow)
                            + 3.0f*u*t*t*(to.y + bow) + t*t*t*to.y;
            for (int b = 0; b < static_cast<int>(buses.size()); ++b) {
                if (b == skipBus) continue;
                if (buses[static_cast<std::size_t>(b)].box.toFloat().expanded(8.0f)
                        .contains(x, y))
                    return true;
            }
        }
        return false;
    };

    float bow = 0.0f;
    for (int step = 0; step < 50 && crosses(bow); ++step) bow += 16.0f;
    return bow;
}

void PatchesView::canvasMouseDown(const juce::MouseEvent& e) {
    const auto pos = e.getPosition();

    if (e.mods.isPopupMenu()) {
        for (const auto& cable : cables) {
            juce::Point<float> to;
            if (cable.kind == Cable::Kind::InputToBus) {
                if (cable.to < 0 || cable.to >= static_cast<int>(buses.size())) continue;
                to = buses[static_cast<std::size_t>(cable.to)].portIn.toFloat();
            } else {
                if (cable.to < 0 || cable.to >= static_cast<int>(outputs.size())) continue;
                to = outputs[static_cast<std::size_t>(cable.to)].port.toFloat();
            }
            const auto from = sourcePoint(cable).toFloat();
            const int skip = cable.kind == Cable::Kind::InputToBus ? cable.to : -1;
            if (distanceToCubic(from, to, pos.toFloat(),
                                cableBow(from, to, skip)) < 7.0f) {
                deleteCable(cable);
                return;
            }
        }
        return;
    }

    // Right-edge socket on an expanded input starts a tap drag.
    for (int i = 0; i < static_cast<int>(inputs.size()); ++i) {
        auto& node = inputs[static_cast<std::size_t>(i)];
        if (node.expanded) {
            for (int s = 0; s < static_cast<int>(node.sockets.size()); ++s) {
                if (node.sockets[static_cast<std::size_t>(s)].pos.getDistanceFrom(pos) <= 9) {
                    dragging = true;
                    dragFromBus = false;
                    dragIndex = i;
                    dragSocket = s;
                    dragPos = pos;
                    return;
                }
            }
        } else if (node.port.getDistanceFrom(pos) <= 9) {
            dragging = true;
            dragFromBus = false;
            dragIndex = i;
            dragSocket = -1;
            dragPos = pos;
            return;
        }
    }
    for (int i = 0; i < static_cast<int>(buses.size()); ++i) {
        if (buses[static_cast<std::size_t>(i)].port.getDistanceFrom(pos) <= 9) {
            dragging = true;
            dragFromBus = true;
            dragIndex = i;
            dragSocket = -1;
            dragPos = pos;
            return;
        }
    }
    // Dragging the bus body reorders the bus column.
    for (int i = 0; i < static_cast<int>(buses.size()); ++i) {
        const auto& box = buses[static_cast<std::size_t>(i)].box;
        if (box.contains(pos)) {
            draggingBus = true;
            dragBusIndex = i;
            dragBusGrabOffset = pos.y - box.getY();
            selection = {Selection::Kind::Bus, buses[static_cast<std::size_t>(i)].id};
            updateStatusText();
            repaint();
            return;
        }
    }

    const auto hit = [pos](const auto& nodes) {
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
            if (nodes[static_cast<std::size_t>(i)].box.contains(pos)) return i;
        return kNoMatch;
    };
    if (const int i = hit(outputs); i >= 0) {
        selection = {Selection::Kind::Output, outputs[static_cast<std::size_t>(i)].id};
        updateStatusText();
        canvasContent.repaint();
        return;
    }
    if (const int i = hit(inputs); i >= 0) {
        // Clicking an input opens its tap card; clicking the card header again
        // closes it. Row clicks only select, so a socket drag never collapses
        // the card underneath the pointer.
        const bool onHeader = pos.y <= inputs[static_cast<std::size_t>(i)].box.getY()
                                          + kCardHeaderH;
        if (!inputs[static_cast<std::size_t>(i)].expanded || onHeader)
            expandedInput = expandedInput == i ? -1 : i;
        selection = {Selection::Kind::Input, inputs[static_cast<std::size_t>(i)].id};
        updateStatusText();
        updateLayout();
        return;
    }
    if (selection.id != 0) {
        selection.id = 0;
        updateStatusText();
        canvasContent.repaint();
    }
}

void PatchesView::canvasMouseDrag(const juce::MouseEvent& e) {
    if (draggingBus) {
        dragPos = e.getPosition();
        if (dragBusIndex >= 0 && dragBusIndex < static_cast<int>(buses.size())) {
            const auto id = buses[static_cast<std::size_t>(dragBusIndex)].id;
            // Move freely while dragging so the stack can be reordered; the
            // spacing rules are applied when the drag ends.
            busY[id] = juce::jmax(0, dragPos.y - dragBusGrabOffset);
            updateLayout();
        }
        return;
    }
    if (!dragging) return;
    dragPos = e.getPosition();
    canvasContent.repaint();
}

void PatchesView::canvasMouseUp(const juce::MouseEvent& e) {
    if (draggingBus) {
        draggingBus = false;
        // The drag may have crossed other pills; settle the whole column in
        // drop order, giving every bus at least one row pitch of separation.
        struct Placed {
            std::uint32_t id;
            int y;
        };
        std::vector<Placed> placed;
        placed.reserve(buses.size());
        for (std::size_t i = 0; i < buses.size(); ++i) {
            const auto id = buses[i].id;
            const auto saved = busY.find(id);
            placed.push_back({id, saved != busY.end()
                                      ? saved->second
                                      : 8 + kRowH / 2 + static_cast<int>(i) * kRowH});
        }
        std::stable_sort(placed.begin(), placed.end(),
                         [](const Placed& a, const Placed& b) { return a.y < b.y; });
        if (!placed.empty()) {
            if (placed.front().y < 0) placed.front().y = 0;
            for (std::size_t i = 1; i < placed.size(); ++i) {
                placed[i].y = juce::jmax(placed[i].y, placed[i - 1].y + kRowH);
            }
            for (const auto& p : placed) busY[p.id] = p.y;
        }
        dragBusIndex = -1;
        updateLayout();
        canvasContent.repaint();
        return;
    }
    if (!dragging) return;
    const auto pos = e.getPosition();
    const auto targetOutput = [&pos, this]() {
        for (int i = 0; i < static_cast<int>(outputs.size()); ++i) {
            const auto& node = outputs[static_cast<std::size_t>(i)];
            if (node.box.contains(pos) || node.port.getDistanceFrom(pos) <= 18) return i;
        }
        return kNoMatch;
    };
    const auto targetBus = [&pos, this]() {
        for (int i = 0; i < static_cast<int>(buses.size()); ++i) {
            const auto& node = buses[static_cast<std::size_t>(i)];
            if (node.box.contains(pos) || node.portIn.getDistanceFrom(pos) <= 18) return i;
        }
        return kNoMatch;
    };

    if (dragFromBus) {
        const int oi = targetOutput();
        if (oi >= 0 && dragIndex >= 0) {
            audioEngine.setBusRoute(buses[static_cast<std::size_t>(dragIndex)].id,
                                    outputs[static_cast<std::size_t>(oi)].id, 0.0f, 0.0f);
        }
    } else if (dragIndex >= 0) {
        const auto& input = inputs[static_cast<std::size_t>(dragIndex)];
        pluginrack::Tap tap;
        if (dragSocket >= 0 && dragSocket < static_cast<int>(input.sockets.size())) {
            tap = input.sockets[static_cast<std::size_t>(dragSocket)].tap;
        } else {
            tap.kind = pluginrack::Tap::Kind::Output;
        }
        if (const int oi = targetOutput(); oi >= 0) {
            audioEngine.connectSend(input.id, outputs[static_cast<std::size_t>(oi)].id, tap);
        } else if (const int bi = targetBus(); bi >= 0) {
            audioEngine.setBusSend(input.id, buses[static_cast<std::size_t>(bi)].id, 0.0f, 0.0f);
            audioEngine.setBusSendTap(input.id, buses[static_cast<std::size_t>(bi)].id, tap);
        }
    }

    dragging = false;
    dragIndex = -1;
    dragSocket = -1;
    rebuild();
}

void PatchesView::deleteCable(const Cable& cable) {
    if (cable.kind == Cable::Kind::InputToBus) {
        if (cable.from < 0 || cable.from >= static_cast<int>(inputs.size())
            || cable.to < 0 || cable.to >= static_cast<int>(buses.size())) return;
        audioEngine.setBusSend(inputs[static_cast<std::size_t>(cable.from)].id,
                               buses[static_cast<std::size_t>(cable.to)].id,
                               AudioEngine::kSendOffDb, AudioEngine::kSendOffDb);
    } else if (cable.kind == Cable::Kind::BusToOutput) {
        if (cable.from < 0 || cable.from >= static_cast<int>(buses.size())
            || cable.to < 0 || cable.to >= static_cast<int>(outputs.size())) return;
        audioEngine.setBusRoute(buses[static_cast<std::size_t>(cable.from)].id,
                                outputs[static_cast<std::size_t>(cable.to)].id,
                                AudioEngine::kSendOffDb, AudioEngine::kSendOffDb);
    } else {
        if (cable.from < 0 || cable.from >= static_cast<int>(inputs.size())
            || cable.to < 0 || cable.to >= static_cast<int>(outputs.size())) return;
        audioEngine.removeSend(inputs[static_cast<std::size_t>(cable.from)].id,
                               outputs[static_cast<std::size_t>(cable.to)].id);
    }
    rebuild();
}

int PatchesView::selectedIndex() const {
    if (selection.id == 0) return -1;
    const std::vector<Node>* nodes = nullptr;
    switch (selection.kind) {
    case Selection::Kind::Input: nodes = &inputs; break;
    case Selection::Kind::Bus: nodes = &buses; break;
    case Selection::Kind::Output: nodes = &outputs; break;
    }
    for (int i = 0; i < static_cast<int>(nodes->size()); ++i)
        if ((*nodes)[static_cast<std::size_t>(i)].id == selection.id) return i;
    return -1;
}

void PatchesView::updateStatusText() {
    header.setStatus(juce::String(inputs.size()) + " inputs - "
                     + juce::String(buses.size()) + " buses - "
                     + juce::String(outputs.size()) + " outputs - "
                     + juce::String(cables.size()) + " patches"
                     + (selectedIndex() >= 0 ? " - showing selected" : ""));
}

void PatchesView::updateLayout() {
    int y = 8;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto& node = inputs[i];
        node.expanded = static_cast<int>(i) == expandedInput;
        if (!node.expanded) {
            node.box = {kSidePad, y, kLabelW, kPillH};
            node.port = {kSidePad + kLabelW + 10, y + kPillH / 2};
            y += kRowH;
        } else {
            const int cardH = kCardHeaderH + kCardPad * 2
                              + static_cast<int>(node.sockets.size()) * kPointH;
            node.box = {kSidePad, y, kLabelW + 20, cardH};
            for (std::size_t s = 0; s < node.sockets.size(); ++s) {
                node.sockets[s].pos = {
                    node.box.getRight() + 10,
                    node.box.getY() + kCardHeaderH + kCardPad
                        + static_cast<int>(s) * kPointH + kPointH / 2};
            }
            node.port = node.sockets.empty() ? juce::Point<int>{node.box.getRight() + 10,
                                                                node.box.getCentreY()}
                                             : node.sockets.back().pos;
            y += cardH + 8;
        }
    }

    const int viewW = juce::jmax(0, canvasViewport.getWidth());
    const int w = juce::jmax(viewW, 3 * (kSidePad + kLabelW) + 60);
    int contentH = juce::jmax(y + 16, canvasViewport.getHeight());
    const int busX = (w - kLabelW) / 2;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        auto& bus = buses[i];
        const auto saved = busY.find(bus.id);
        // Default sits in the gap between input rows, not on a row.
        const int ry = saved != busY.end() ? saved->second
                                           : 8 + kRowH / 2 + static_cast<int>(i) * kRowH;
        bus.box = {busX, ry, kLabelW, kPillH};
        bus.portIn = {busX - 10, ry + kPillH / 2};
        bus.port = {busX + kLabelW + 10, ry + kPillH / 2};
        contentH = juce::jmax(contentH, ry + kPillH + 16);
    }
    contentH = juce::jmax(contentH, 8 + static_cast<int>(outputs.size()) * kRowH + 16);
    canvasContent.setSize(w, contentH);
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const int ry = 8 + static_cast<int>(i) * kRowH;
        outputs[i].box = {w - kSidePad - kLabelW, ry, kLabelW, kPillH};
        outputs[i].port = {w - kSidePad - kLabelW - 10, ry + kPillH / 2};
    }
    canvasContent.repaint();
}

void PatchesView::resized() {
    auto area = getLocalBounds().reduced(theme::ui::kPagePad);
    header.setBounds(area.removeFromTop(theme::ui::kHeaderH));
    area.removeFromTop(theme::ui::kHeaderGap);
    canvasViewport.setBounds(area);
    updateLayout();
}

void PatchesView::rebuild() {
    inputs.clear();
    buses.clear();
    outputs.clear();
    cables.clear();

    const auto& channels = audioEngine.getChannels();
    const auto& outList = audioEngine.getOutputs();
    for (const auto& ch : channels) {
        Node node;
        node.id = ch->id;
        node.name = ch->name;
        node.hue = ch->colourHue;
        Socket inputSocket;
        inputSocket.tap.kind = pluginrack::Tap::Kind::Input;
        inputSocket.label = "Input";
        node.sockets.push_back(inputSocket);
        for (std::size_t i = 0; i < ch->chain.size(); ++i) {
            const auto& item = ch->chain[i];
            // A tap names a row by (id, type). Built-in families mint ids
            // independently, so one strip can hold a Gate row and a Feedback
            // Suppressor row that are both id 1; the type keeps them distinct,
            // so every row is a source and no row disappears from the card.
            if (static_cast<int>(i) != pluginrack::firstChainItemIndex(ch->chain, item))
                continue;
            Socket socket;
            socket.tap.kind = pluginrack::Tap::Kind::Item;
            socket.tap.itemId = item.id;
            socket.tap.hasItemType = true;
            socket.tap.itemType = item.type;
            socket.label = chainItemLabel(*ch, item);
            node.sockets.push_back(socket);
        }
        Socket outputSocket;
        outputSocket.tap.kind = pluginrack::Tap::Kind::Output;
        outputSocket.label = "Output";
        node.sockets.push_back(outputSocket);
        inputs.push_back(std::move(node));
    }
    for (const auto& bus : audioEngine.getBuses()) {
        Node node;
        node.id = bus->id;
        node.name = bus->name;
        node.hue = bus->colourHue;
        buses.push_back(std::move(node));
    }
    for (const auto& out : outList) {
        Node node;
        node.id = out->id;
        node.name = out->name;
        node.hue = out->colourHue;
        outputs.push_back(std::move(node));
    }

    const auto indexOfChannel = [&channels](std::uint32_t id) {
        for (int i = 0; i < static_cast<int>(channels.size()); ++i)
            if (channels[static_cast<std::size_t>(i)]->id == id) return i;
        return kNoMatch;
    };
    const auto indexOfBus = [this](std::uint32_t id) {
        const auto& list = audioEngine.getBuses();
        for (int i = 0; i < static_cast<int>(list.size()); ++i)
            if (list[static_cast<std::size_t>(i)]->id == id) return i;
        return kNoMatch;
    };
    const auto indexOfOutput = [&outList](std::uint32_t id) {
        for (int i = 0; i < static_cast<int>(outList.size()); ++i)
            if (outList[static_cast<std::size_t>(i)]->id == id) return i;
        return kNoMatch;
    };

    // One cable per active send (gain above the -60 dB floor), coloured by
    // the destination strip's identity colour.
    for (int ci = 0; ci < static_cast<int>(channels.size()); ++ci) {
        for (int oi = 0; oi < static_cast<int>(outList.size()); ++oi) {
            const auto channelId = channels[static_cast<std::size_t>(ci)]->id;
            const auto outputId = outList[static_cast<std::size_t>(oi)]->id;
            const float gain = audioEngine.getSendGainL(channelId, outputId);
            if (gain <= AudioEngine::kSendOffDb) continue;
            Cable cable;
            cable.kind = Cable::Kind::InputToOutput;
            cable.from = ci;
            cable.to = oi;
            cable.tap = audioEngine.getSendTap(channelId, outputId);
            cable.colour = theme::stripColour(
                outList[static_cast<std::size_t>(oi)]->colourHue);
            cables.push_back(cable);
        }
    }
    for (const auto& send : audioEngine.getBusSends()) {
        if (send->sendGainDbL.get() <= AudioEngine::kSendOffDb
            && send->sendGainDbR.get() <= AudioEngine::kSendOffDb)
            continue;
        const int ci = indexOfChannel(send->channelId);
        const int bi = indexOfBus(send->busId);
        if (ci < 0 || bi < 0) continue;
        Cable cable;
        cable.kind = Cable::Kind::InputToBus;
        cable.from = ci;
        cable.to = bi;
        cable.tap = audioEngine.getBusSendTap(send->channelId, send->busId);
        cable.colour = theme::stripColour(
            audioEngine.getBuses()[static_cast<std::size_t>(bi)]->colourHue);
        cables.push_back(cable);
    }
    for (const auto& route : audioEngine.getBusRoutes()) {
        if (route->sendGainDbL.get() <= AudioEngine::kSendOffDb
            && route->sendGainDbR.get() <= AudioEngine::kSendOffDb)
            continue;
        const int bi = indexOfBus(route->busId);
        const int oi = indexOfOutput(route->outputId);
        if (bi < 0 || oi < 0) continue;
        Cable cable;
        cable.kind = Cable::Kind::BusToOutput;
        cable.from = bi;
        cable.to = oi;
        cable.colour = theme::stripColour(
            outList[static_cast<std::size_t>(oi)]->colourHue);
        cables.push_back(cable);
    }

    if (selection.id != 0 && selectedIndex() < 0)
        selection.id = 0;

    updateStatusText();
    updateLayout();
}
