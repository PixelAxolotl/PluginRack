#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <pluginrack/Scene.h>

class SavesView final : public juce::Component {
public:
    // onRestore applies a parsed scene to the engine and returns plugin
    // load errors. It runs synchronously on the message thread inside the
    // host: SavesView never touches the engine itself, because restoreScene
    // instantiates plugins and destroys JUCE components that must not be
    // mutated off that thread.
    explicit SavesView(std::function<juce::String(const pluginrack::Scene&)> onRestore);
    ~SavesView() override;

    void rebuild();
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class SaveRowComponent final : public juce::Component {
    public:
        SaveRowComponent(const juce::String& name, bool isDefault, bool isSelected);

        void setDefaultActive(bool isDefault);
        void setSelected(bool isSelected);
        void setLoadEnabled(bool enabled);

        void resized() override;
        void paint(juce::Graphics& g) override;
        void mouseUp(const juce::MouseEvent& event) override;

        std::function<void()> onSelect;
        std::function<void()> onLoad;
        std::function<void()> onSetDefault;
        std::function<void()> onRename;
        std::function<void()> onDelete;

    private:
        juce::String saveName;
        bool isDefault = false;
        bool isSelected = false;

        juce::Label nameLabel;
        juce::TextButton defButton{"DEF"};
        juce::TextButton renameButton{"Edit"};
        juce::TextButton deleteButton{"Delete"};
        juce::TextButton loadButton{"Load"};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SaveRowComponent)
    };

    void layoutRows();
    void updateSelection();
    void setLoading(bool isLoading);
    void loadSave(const juce::String& name);
    void promptRename(const juce::String& oldName);
    void confirmDelete(const juce::String& name);

    std::function<juce::String(const pluginrack::Scene&)> onRestore;

    juce::Label titleLabel;
    juce::Label statusLabel;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<SaveRowComponent>> rows;
    int selectedRow = -1;
    std::atomic<bool> loading_{false};

    static constexpr int kRowHeight = 64;
    static constexpr int kRowGap = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SavesView)
};
