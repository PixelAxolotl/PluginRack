#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "AudioEngine.h"
#include "InsertMiniView.h"
#include "MiniButton.h"
#include "PluginMenu.h"
#include "Theme.h"
#include "pluginrack/ChainOrder.h"

// Release-only hook fired synchronously BEFORE erasePlugin frees the
// processor, so a docked editor for the doomed insert is destroyed while
// still alive.
//
// A full onStructureChanged round-trip cannot be used here: it would
// rebuild the rack and re-dock the doomed insert before the erase runs.
// The view router owns the only docked editor, so it assigns this hook;
// it stays empty in unit tests, where there is no router.
inline std::function<void()>& pluginRemovingHook() {
    static std::function<void()> hook;
    return hook;
}

// Every strip renders the same 8-slot chain: 3 built-in effects followed by
// 5 VST slots. Unfilled VST slots are the add button rather than a separate
// control, so the list keeps a constant length while plugins are added.
inline constexpr int kChainBuiltInSlots = 3;
inline constexpr int kChainVstSlots = 5;
inline constexpr int kChainSlotCount = kChainBuiltInSlots + kChainVstSlots;

// One built-in effect as a card switch.
struct CardSwitch {
    pluginrack::ChainItem item;
    bool bypassed = false;
    juce::String label;
    juce::String tooltip;
};

class IInsertHost;

// One add-plugin flow for every insert host: the plugin library menu anchored
// to the clicked slot, then insertPlugin with its immediate error surfaced.
void openPluginPicker(IInsertHost& host, juce::Component& anchor);

class IInsertHost {
public:
    virtual ~IInsertHost() = default;
    // Starts an asynchronous load: returns an immediate validation error, or an
    // empty string when the plugin is being instantiated on the load worker.
    // pluginUniqueId names the class inside a multi-class bundle; 0 means "the
    // bundle's first class". The view refreshes itself from the completion.
    virtual juce::String insertPlugin(const juce::File&, int pluginUniqueId = 0) = 0;
    virtual void erasePlugin(std::uint32_t insertId) = 0;
    virtual void setPluginBypass(std::uint32_t insertId, bool bypassed) = 0;
    virtual void refreshAfterInsertChange() = 0;
    virtual void openPicker(juce::Component& anchor) { openPluginPicker(*this, anchor); }
    virtual void reorderInsert(std::uint32_t /*insertId*/, int /*targetIndex*/) {}
    virtual void reorderChainItem(pluginrack::ChainItem /*item*/, int /*targetIndex*/) {}
    virtual void previewChainItem(int /*targetIndex*/) {}
    virtual void clearChainPreview() {}
    // Maps a point in the slot parent's coordinates to a chain insertion index.
    // Each view owns its own row pitch, so the slot must not assume one.
    virtual int chainDropIndexAt(juce::Point<int> /*parentPoint*/) { return -1; }
    virtual void setBuiltInBypass(pluginrack::ChainItem /*item*/, bool /*bypassed*/) {}
    virtual void activateBuiltIn(pluginrack::ChainItem /*item*/) {}
    // Channel view overrides this to focus the clicked slot inline (returns
    // true, skipping the floating editor). Default false keeps showEditor().
    virtual bool insertActivated(AudioEngine::InsertSlot* /*insert*/) { return false; }
};

inline void openPluginPicker(IInsertHost& host, juce::Component& anchor) {
    // The menu outlives this click, so the host is tracked through its other
    // base class: every host is a Component, and the cast back is checked.
    juce::Component::SafePointer<juce::Component> safeHost(dynamic_cast<juce::Component*>(&host));
    PluginMenu::showAddPluginMenu(anchor, [safeHost](const juce::File& file, int pluginUniqueId) {
        if (safeHost == nullptr) return;
        auto* target = dynamic_cast<IInsertHost*>(safeHost.getComponent());
        if (target == nullptr) return;
        const auto error = target->insertPlugin(file, pluginUniqueId);
        if (error.isNotEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "PluginRack", error);
        }
    });
}

class InsertSlotComponent final : public juce::Component, public juce::SettableTooltipClient {
public:
    InsertSlotComponent(IInsertHost& hostRef, int index)
        : host(hostRef), slotIndex(index),
          hostComponent(dynamic_cast<juce::Component*>(&hostRef)) {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setTooltip("Click to load a VST3 plugin");

        indexLabel.setText(juce::String(index + 1), juce::dontSendNotification);
        indexLabel.setJustificationType(juce::Justification::centred);
        indexLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
        indexLabel.setColour(juce::Label::textColourId, theme::dim());
        indexLabel.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(indexLabel);

        nameLabel.setJustificationType(juce::Justification::centred);
        nameLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
        nameLabel.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(nameLabel);

        bypassButton.setClickAction([this] {
            if (builtIn) {
                const bool next = !bypassButton.getToggleState();
                bypassButton.setToggleState(next);
                host.setBuiltInBypass(chainItem, next);
                return;
            }
            if (insert == nullptr) return;
            const bool next = !bypassButton.getToggleState();
            bypassButton.setToggleState(next);
            host.setPluginBypass(insert->id, next);
        });
        bypassButton.setTooltip("Bypass this plugin");
        addAndMakeVisible(bypassButton);

        removeButton.setClickAction([this] {
            if (insert == nullptr) return;
            const std::uint32_t doomedId = insert->id;
            // Drop the freed pointer before the synchronous erase below:
            // the engine destroys the insert, and the refresh that rebinds
            // these slots only runs async afterwards. Greys out at once, so
            // a second click in that window is a guarded no-op.
            setInsert(nullptr);
            // Release-only, synchronous: destroys the rack's docked editor
            // (if it shows this insert) while the processor is still alive.
            // Must run before erasePlugin, and must not rebuild (a rebuild
            // would re-dock the doomed insert before the erase).
            if (auto release = pluginRemovingHook()) release();
            host.erasePlugin(doomedId);
            if (hostComponent != nullptr) {
                juce::Component::SafePointer<juce::Component> safe(hostComponent);
                juce::MessageManager::callAsync([safe] {
                    if (auto* comp = safe.getComponent()) {
                        if (auto* h = dynamic_cast<IInsertHost*>(comp))
                            h->refreshAfterInsertChange();
                    }
                });
            }
        });
        removeButton.setTooltip("Remove this plugin");
        addAndMakeVisible(removeButton);

        // A card can hold more than one effect (the Dynamics card carries the
        // EQ and the compressor), so it carries one switch per effect instead
        // of the single bypass button. Each switch follows the strip
        // convention: left-click bypasses, right-click opens that effect.
        pairFirstButton.setFontSize(9.0f);
        pairFirstButton.setClickAction([this] {
            const bool next = !pairFirstButton.getToggleState();
            pairFirstButton.setToggleState(next);
            host.setBuiltInBypass(pairFirst.item, next);
        });
        pairFirstButton.onRightClick = [this] { host.activateBuiltIn(pairFirst.item); };
        addChildComponent(pairFirstButton);

        pairSecondButton.setFontSize(9.0f);
        pairSecondButton.setClickAction([this] {
            const bool next = !pairSecondButton.getToggleState();
            pairSecondButton.setToggleState(next);
            host.setBuiltInBypass(pairSecond.item, next);
        });
        pairSecondButton.onRightClick = [this] { host.activateBuiltIn(pairSecond.item); };
        addChildComponent(pairSecondButton);
    }

    void setInsert(AudioEngine::InsertSlot* newInsert) {
        insert = newInsert;
        chainItem = insert == nullptr ? pluginrack::ChainItem{} :
                                        pluginrack::ChainItem{pluginrack::ChainItemType::Insert, insert->id};
        builtIn = false;
        addSlot = false;
        addSlotPrimary = false;
        paired = false;
        pairFirstButton.setVisible(false);
        pairSecondButton.setVisible(false);
        removeButton.setVisible(true);
        bypassButton.setVisible(true);
        if (insert != nullptr) {
            nameLabel.setText(insert->pluginName.isNotEmpty() ? insert->pluginName
                                                              : juce::File(insert->pluginPath).getFileName(),
                              juce::dontSendNotification);
            bypassButton.setToggleState(insert->bypassed.get());
            bypassButton.setEnabledState(true);
            removeButton.setEnabledState(true);
            nameLabel.setColour(juce::Label::textColourId, theme::text());
            nameLabel.setTooltip(insert->pluginName.isNotEmpty() ? insert->pluginName : insert->pluginPath);
            setTooltip("Drag the dots to reorder - click to open the plugin editor");
        } else {
            nameLabel.setText("empty", juce::dontSendNotification);
            nameLabel.setColour(juce::Label::textColourId, theme::dim());
            nameLabel.setTooltip("Click to load a VST3 plugin");
            bypassButton.setToggleState(false);
            bypassButton.setEnabledState(false);
            removeButton.setEnabledState(false);
            setTooltip("Click to load a VST3 plugin");
        }
        repaint();
    }

    // An unfilled VST slot: the row is the add control until a plugin lands
    // in it, then setInsert() turns it into a normal chain row. Only the
    // first empty slot names itself; the rest stay quiet until hovered, so
    // five spare slots do not shout over the plugins that are actually there.
    void setAddSlot(bool primary) {
        insert = nullptr;
        builtIn = false;
        addSlot = true;
        addSlotPrimary = primary;
        paired = false;
        pairFirstButton.setVisible(false);
        pairSecondButton.setVisible(false);
        nameLabel.setText(primary ? "+ Add VST3" : juce::String(),
                          juce::dontSendNotification);
        nameLabel.setColour(juce::Label::textColourId, theme::violet().withAlpha(0.75f));
        nameLabel.setTooltip("Add a VST3 plugin to this slot");
        bypassButton.setVisible(false);
        removeButton.setVisible(false);
        setTooltip("Click to add a VST3 plugin");
        repaint();
    }

    void setBuiltIn(pluginrack::ChainItem item, const juce::String& title, bool bypassed) {
        insert = nullptr;
        chainItem = item;
        builtIn = true;
        addSlot = false;
        addSlotPrimary = false;
        paired = false;
        pairFirstButton.setVisible(false);
        pairSecondButton.setVisible(false);
        bypassButton.setVisible(true);
        nameLabel.setText(title, juce::dontSendNotification);
        nameLabel.setColour(juce::Label::textColourId, theme::text());
        nameLabel.setTooltip(title);
        bypassButton.setToggleState(bypassed);
        bypassButton.setEnabledState(true);
        removeButton.setVisible(false);
        setTooltip("Drag the dots to reorder - click to open effect settings");
        repaint();
    }

    // Rack cards carry a read-only mini view under the row; strips that do not
    // ask for one keep the plain 36 px row.
    void setMiniView(std::unique_ptr<InsertMiniView> view) {
        mini = std::move(view);
        if (mini != nullptr) {
            addAndMakeVisible(*mini);
            mini->setBounds(getLocalBounds().withTrimmedTop(kHeaderHeight));
        }
        resized();
        repaint();
    }

    InsertMiniView* miniView() const { return mini.get(); }

    // One card for two built-in effects: both chain rows are shown as a single
    // unit with a bypass switch each, because the two effects share one editor.
    // bodyItem is what a plain click on the row opens.
    void setBuiltInPair(const juce::String& title, pluginrack::ChainItem bodyItem,
                        const CardSwitch& first, const CardSwitch& second) {
        insert = nullptr;
        chainItem = bodyItem;
        builtIn = true;
        addSlot = false;
        paired = true;
        pairFirst = first;
        pairSecond = second;
        pairFirstButton.setLabel(first.label);
        pairFirstButton.setTooltip(first.tooltip);
        pairSecondButton.setLabel(second.label);
        pairSecondButton.setTooltip(second.tooltip);
        nameLabel.setText(title, juce::dontSendNotification);
        nameLabel.setColour(juce::Label::textColourId, theme::text());
        nameLabel.setTooltip(title);
        bypassButton.setVisible(false);
        removeButton.setVisible(false);
        pairFirstButton.setVisible(true);
        pairSecondButton.setVisible(true);
        setPairBypassed(first.bypassed, second.bypassed);
        setTooltip("Click to open the " + title.toLowerCase() + " panel - "
                   "the switches bypass each effect");
        resized();
        repaint();
    }

    void setPairBypassed(bool firstBypassed, bool secondBypassed) {
        pairFirstButton.setToggleState(firstBypassed);
        pairSecondButton.setToggleState(secondBypassed);
    }

    // Live bypass state for a built-in row: the EQ and dynamics panels switch
    // the effect on without the list being rebuilt.
    void setBuiltInBypassed(bool bypassed) {
        if (bypassButton.getToggleState() == bypassed) return;
        bypassButton.setToggleState(bypassed);
    }

    // The insert the main bay is currently showing, so the card can say so.
    void setSelected(bool isSelected) {
        if (selected == isSelected) return;
        selected = isSelected;
        repaint();
    }

    int preferredHeight() const {
        return kHeaderHeight + (mini != nullptr ? InsertMiniView::kHeight : 0);
    }

    static constexpr int kHeaderHeight = 36;

    void resized() override {
        auto area = getLocalBounds().reduced(3, 1);
        indexLabel.setVisible(false);
        area.removeFromLeft(20);
        if (mini != nullptr)
            mini->setBounds(getLocalBounds().withTrimmedTop(kHeaderHeight).reduced(2, 0));
        // Mixer strips use compact rows (14-20 px), while rack cards reserve
        // a 36 px header above their preview. Keep the label inside the actual
        // row so compact text is vertically centered instead of clipped.
        const int headerHeight = mini != nullptr ? kHeaderHeight : getHeight();
        area = area.withHeight(juce::jmax(0, headerHeight - 2));
        if (addSlot) {
            nameLabel.setBounds(area);
            return;
        }
        if (paired) {
            // Square switches: both the rack card (36 px header) and the
            // compact mixer row give them one side, centred vertically.
            const int side = juce::jlimit(12, 24, area.getHeight());
            auto buttons = area.removeFromRight(2 * side + 3)
                               .withSizeKeepingCentre(2 * side + 3, side);
            pairSecondButton.setBounds(buttons.removeFromRight(side));
            buttons.removeFromRight(3);
            pairFirstButton.setBounds(buttons.removeFromRight(side));
            area.removeFromRight(4);
            nameLabel.setBounds(area);
            return;
        }
        removeButton.setBounds(area.removeFromRight(18));
        area.removeFromRight(2);
        bypassButton.setBounds(area.removeFromRight(18));
        area.removeFromRight(3);
        nameLabel.setBounds(area);
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        const bool filled = insert != nullptr || builtIn;
        juce::Colour face = filled ? theme::strip() : theme::control().withAlpha(0.45f);
        if (hovered && insert == nullptr) face = theme::control().withAlpha(0.75f);
        // The row being dragged lifts off the stack: a lighter face and a
        // heavier accent edge read as "in flight" next to the cyan drop bar.
        if (dragging) face = theme::control();
        g.setColour(face);
        g.fillRoundedRectangle(r, 3.0f);

        // Six-dot handle, centred in the gutter resized() leaves for it. Rack
        // cards are taller than their header because of the mini view, so the
        // grip follows the header row: centring it on the whole card would put
        // the dots under the preview, where nobody can see or grab them.
        constexpr float dotSize = 2.5f;
        constexpr float dotGapX = 4.0f;
        constexpr float dotGapY = 4.0f;
        constexpr float gridW = dotSize * 2.0f + dotGapX;
        constexpr float gridH = dotSize * 3.0f + dotGapY * 2.0f;
        const float gridLeft = 3.0f + (20.0f - gridW) * 0.5f;
        const float headerHeight = static_cast<float>(mini == nullptr ? getHeight() : kHeaderHeight);
        const float gridTop = (headerHeight - gridH) * 0.5f;
        // An empty VST slot has nothing to reorder, so it carries no handle.
        if (!addSlot) {
            g.setColour(theme::dim());
            for (int dotRow = 0; dotRow < 3; ++dotRow)
                for (int dotCol = 0; dotCol < 2; ++dotCol)
                    g.fillEllipse(gridLeft + dotCol * (dotSize + dotGapX),
                                  gridTop + dotRow * (dotSize + dotGapY), dotSize, dotSize);
        }

        if (filled) {
            g.setColour(builtIn ? theme::orange() : theme::violet());
            g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, dragging ? 2.0f : 1.0f);
        } else if (addSlot) {
            g.setColour(theme::violet().withAlpha(hovered || addSlotPrimary ? 0.45f : 0.18f));
            g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
        } else {
            g.setColour(theme::border());
            g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
        }

        // The card whose insert is in the center bay reads as selected, so the
        // list never leaves you guessing which editor is showing.
        if (selected) {
            g.setColour(theme::cyan());
            g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 2.0f);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override {
        // The handle lives in the header row only: the mini view below it is
        // for looking at, and clicking it must open the editor, not reorder.
        dragging = inHandle(e);
        dragOffset = e.y;
        repaint();
        if (insert == nullptr && !builtIn) {
            host.openPicker(*this);
        } else if (builtIn && !dragging) {
            host.activateBuiltIn(chainItem);
        } else if (!dragging && !host.insertActivated(insert)) {
            insert->pluginHost.showEditor();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (!dragging || hostComponent == nullptr) return;
        const auto point = getParentComponent()->getLocalPoint(this, e.getPosition());
        const int target = host.chainDropIndexAt(point);
        if (target < 0) return;
        pendingTarget = target;
        host.previewChainItem(pendingTarget);
    }

    void mouseUp(const juce::MouseEvent&) override {
        if (dragging && pendingTarget >= 0)
            host.reorderChainItem(chainItem, pendingTarget);
        host.clearChainPreview();
        dragging = false;
        pendingTarget = -1;
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override {
        const bool onHandle = inHandle(e);
        setMouseCursor(onHandle ? juce::MouseCursor::DraggingHandCursor
                                : juce::MouseCursor::PointingHandCursor);
    }

    void mouseEnter(const juce::MouseEvent&) override {
        hovered = true;
        if (addSlot && !addSlotPrimary)
            nameLabel.setText("+ Add VST3", juce::dontSendNotification);
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override {
        hovered = false;
        if (addSlot && !addSlotPrimary)
            nameLabel.setText(juce::String(), juce::dontSendNotification);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        repaint();
    }

private:
    bool inHandle(const juce::MouseEvent& e) const {
        return e.x < 28 && e.y < kHeaderHeight && (insert != nullptr || builtIn);
    }

    IInsertHost& host;
    juce::Component* hostComponent = nullptr;
    int slotIndex = 0;
    AudioEngine::InsertSlot* insert = nullptr;
    pluginrack::ChainItem chainItem;
    bool builtIn = false;
    bool addSlot = false;
    bool addSlotPrimary = false;
    bool hovered = false;
    bool dragging = false;
    bool selected = false;
    std::unique_ptr<InsertMiniView> mini;
    int dragOffset = 0;
    int pendingTarget = -1;
    juce::Label indexLabel;
    juce::Label nameLabel;
    MiniButton bypassButton{"B"};
    MiniButton removeButton{juce::String::fromUTF8("\xc3\x97")};
    MiniButton pairFirstButton{"EQ"};
    MiniButton pairSecondButton{"C"};
    CardSwitch pairFirst;
    CardSwitch pairSecond;
    bool paired = false;
};
