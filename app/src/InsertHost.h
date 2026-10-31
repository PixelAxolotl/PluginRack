#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>

#include "AudioEngine.h"
#include "MiniButton.h"
#include "Theme.h"

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

class IInsertHost {
public:
    virtual ~IInsertHost() = default;
    virtual juce::String insertPlugin(const juce::File&) = 0;
    virtual void erasePlugin(std::uint32_t insertId) = 0;
    virtual void setPluginBypass(std::uint32_t insertId, bool bypassed) = 0;
    virtual void refreshAfterInsertChange() = 0;
    virtual void openPicker() = 0;
    // Channel view overrides this to focus the clicked slot inline (returns
    // true, skipping the floating editor). Default false keeps showEditor().
    virtual bool insertActivated(AudioEngine::InsertSlot* /*insert*/) { return false; }
};

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

        nameLabel.setJustificationType(juce::Justification::centredLeft);
        nameLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
        nameLabel.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(nameLabel);

        bypassButton.setClickAction([this] {
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
    }

    void setInsert(AudioEngine::InsertSlot* newInsert) {
        insert = newInsert;
        if (insert != nullptr) {
            nameLabel.setText(insert->pluginName.isNotEmpty() ? insert->pluginName
                                                              : juce::File(insert->pluginPath).getFileName(),
                              juce::dontSendNotification);
            bypassButton.setToggleState(insert->bypassed.get());
            bypassButton.setEnabledState(true);
            removeButton.setEnabledState(true);
            nameLabel.setColour(juce::Label::textColourId, theme::text());
            nameLabel.setTooltip(insert->pluginName.isNotEmpty() ? insert->pluginName : insert->pluginPath);
            setTooltip("Click to open the plugin editor");
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

    void resized() override {
        auto area = getLocalBounds().reduced(3, 1);
        indexLabel.setBounds(area.removeFromLeft(14));
        removeButton.setBounds(area.removeFromRight(18));
        area.removeFromRight(2);
        bypassButton.setBounds(area.removeFromRight(18));
        area.removeFromRight(3);
        nameLabel.setBounds(area);
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        juce::Colour face = insert != nullptr ? theme::strip() : theme::control().withAlpha(0.45f);
        if (hovered && insert == nullptr) face = theme::control().withAlpha(0.75f);
        g.setColour(face);
        g.fillRoundedRectangle(r, 3.0f);
        if (insert != nullptr) {
            g.setColour(theme::violet().withAlpha(0.6f));
            g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
        } else {
            g.setColour(theme::border());
            g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
        }
    }

    void mouseDown(const juce::MouseEvent&) override {
        if (insert == nullptr) {
            host.openPicker();
        } else if (!host.insertActivated(insert)) {
            insert->pluginHost.showEditor();
        }
    }

    void mouseEnter(const juce::MouseEvent&) override {
        hovered = true;
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override {
        hovered = false;
        repaint();
    }

private:
    IInsertHost& host;
    juce::Component* hostComponent = nullptr;
    int slotIndex = 0;
    AudioEngine::InsertSlot* insert = nullptr;
    bool hovered = false;
    juce::Label indexLabel;
    juce::Label nameLabel;
    MiniButton bypassButton{"B"};
    MiniButton removeButton{juce::String::fromUTF8("\xc3\x97")};
};
