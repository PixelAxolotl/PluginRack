#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>

#include "AudioEngine.h"
#include "InsertHost.h"
#include "LevelMeter.h"
#include "Theme.h"

// Rack-first workspace: one chain per channel/output. Each chain keeps a
// fixed header (name, source identity, meter, mute/solo, routing summary)
// beside horizontally scrolling plugin modules. Module rendering reuses
// InsertSlotComponent, mirroring the mixer strips.
class RackView final : public juce::Component {
public:
    explicit RackView(AudioEngine& engine);

    void resized() override;
    void rebuild();

private:
    class ChainRow final : public juce::Component, public IInsertHost {
    public:
        ChainRow(AudioEngine& engine, AudioEngine::Channel& channelState);
        ChainRow(AudioEngine& engine, AudioEngine::Output& outputState);

        void resized() override;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent&) override;

        void refresh();
        void setSelected(bool isSelected);

        bool isOutput() const { return output != nullptr; }
        std::uint32_t chainId() const { return channel != nullptr ? channel->id : output->id; }

        // IInsertHost
        juce::String insertPlugin(const juce::File& file) override;
        void erasePlugin(std::uint32_t insertId) override;
        void setPluginBypass(std::uint32_t insertId, bool bypassed) override;
        void refreshAfterInsertChange() override;
        void openPicker() override;

        std::function<void(ChainRow*)> onSelected;

    private:
        void initHeader();
        void refreshModules();
        void layoutModules();
        juce::String buildRoutingSummary() const;

        AudioEngine& audioEngine;
        AudioEngine::Channel* channel = nullptr;
        AudioEngine::Output* output = nullptr;
        bool selected = false;

        juce::Label nameLabel;
        juce::Label identityLabel;
        juce::Label routingLabel;
        std::unique_ptr<LevelMeter> meter;
        juce::TextButton muteButton{"Mute"};
        juce::TextButton soloButton{"Solo"};
        juce::Viewport moduleViewport;
        juce::Component moduleContent;
        juce::OwnedArray<InsertSlotComponent> slots;
        juce::Label emptyLabel;
        juce::TextButton addVstButton{"+ Add VST3"};
        std::unique_ptr<juce::FileChooser> fileChooser;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainRow)
    };

    void selectRow(ChainRow* row);

    AudioEngine& audioEngine;
    juce::Viewport listViewport;
    juce::Component listContent;
    juce::OwnedArray<ChainRow> rows;
    juce::Label emptyLabel;

    bool hasSelection = false;
    bool selectedIsOutput = false;
    std::uint32_t selectedId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RackView)
};
