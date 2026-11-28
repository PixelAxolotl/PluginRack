#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <pluginrack/Scene.h>

#include "PageHeader.h"

class SavesView final : public juce::Component {
public:
    // onRestore starts a staged restore and returns an immediate error; the
    // completion it receives gets the plugin load errors once the engine has
    // committed the new strips. All of it runs on the message thread: the
    // engine instantiates plugins and destroys JUCE components, which must
    // not happen off that thread.
    // onLoaded tells the host which save is now active, so the toolbar Save
    // button overwrites it instead of asking for a new name.
    // onSaveIdentityChanged reports a rename (old, new) or a delete (old, {})
    // so the host can keep the toolbar's active save name in step.
    explicit SavesView(std::function<juce::String(const pluginrack::Scene&, std::function<void(const juce::String&)>)>
                           onRestore,
                       std::function<void(const juce::String&)> onLoaded = {},
                       std::function<void(const juce::String&, const juce::String&)>
                           onSaveIdentityChanged = {});
    ~SavesView() override;

    void rebuild();
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class SaveRowComponent final : public juce::Component {
    public:
        SaveRowComponent(const juce::String& displayName, const juce::File& file, bool isDefault,
                         bool isSelected, bool readOnly);

        void setDefaultActive(bool isDefault);
        void setSelected(bool isSelected);
        void setLoadEnabled(bool enabled);
        void setActionsEnabled(bool enabled);

        void resized() override;
        void paint(juce::Graphics& g) override;
        void mouseUp(const juce::MouseEvent& event) override;
        bool keyPressed(const juce::KeyPress& key) override;

        std::function<void()> onSelect;
        std::function<void()> onLoad;
        std::function<void()> onSetDefault;
        std::function<void()> onRename;
        std::function<void()> onDelete;

    private:
        juce::String saveName;
        bool isDefault = false;
        bool isSelected = false;
        bool isReadOnly = false;

        juce::Label nameLabel;
        juce::Label metaLabel;
        juce::TextButton defButton{"DEF"};
        juce::TextButton renameButton{"Edit"};
        juce::TextButton deleteButton{"Delete"};
        juce::TextButton loadButton{"Load"};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SaveRowComponent)
    };

    // A named save, or the previous session's autosave shown read-only.
    struct Entry {
        juce::String name;
        juce::File file;
        bool readOnly = false;
    };

    void layoutRows();
    void updateSelection();
    void setLoading(bool isLoading);
    void loadSave(const Entry& entry);
    void promptRename(const juce::String& oldName);
    void confirmDelete(const juce::String& name);

    std::function<juce::String(const pluginrack::Scene&, std::function<void(const juce::String&)>)>
        onRestore;
    std::function<void(const juce::String&)> onLoaded;
    std::function<void(const juce::String&, const juce::String&)> onSaveIdentityChanged;

    PageHeader header{"Saves"};
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<SaveRowComponent>> rows;
    std::vector<Entry> entries;
    int selectedRow = -1;
    juce::String selectedName;
    std::atomic<bool> loading_{false};

    static constexpr int kRowHeight = 64;
    static constexpr int kRowGap = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SavesView)
};
