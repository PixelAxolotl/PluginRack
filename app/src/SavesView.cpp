#include "SavesView.h"

#include "SceneBridge.h"
#include "Theme.h"

SavesView::SaveRowComponent::SaveRowComponent(const juce::String& displayName, const juce::File& file,
                                              bool isDefaultIn, bool isSelectedIn, bool readOnlyIn)
    : saveName(displayName)
    , isDefault(isDefaultIn)
    , isSelected(isSelectedIn)
    , isReadOnly(readOnlyIn) {
    nameLabel.setText(saveName, juce::dontSendNotification);
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    nameLabel.setFont(theme::ui::itemFont());
    nameLabel.setColour(juce::Label::textColourId, isReadOnly ? theme::dim() : theme::text());
    nameLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel);

    const auto modified = file.getLastModificationTime();
    if (isReadOnly) {
        // The autosave is written on exit; its timestamp is the last session.
        metaLabel.setText("Autosave - previous session", juce::dontSendNotification);
    } else {
        metaLabel.setText(modified.toMilliseconds() != 0
                              ? "Modified " + modified.toString(true, false)
                              : "",
                          juce::dontSendNotification);
    }
    metaLabel.setJustificationType(juce::Justification::centredRight);
    metaLabel.setFont(theme::ui::captionFont());
    metaLabel.setColour(juce::Label::textColourId, theme::dim());
    metaLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(metaLabel);

    defButton.setClickingTogglesState(true);
    defButton.setToggleState(isDefault, juce::dontSendNotification);
    defButton.setTooltip("Load this save at startup");
    defButton.onClick = [this] { if (onSetDefault) onSetDefault(); };
    addAndMakeVisible(defButton);

    renameButton.setTooltip("Rename this save");
    renameButton.onClick = [this] { if (onRename) onRename(); };
    addAndMakeVisible(renameButton);

    deleteButton.setTooltip("Delete this save");
    deleteButton.onClick = [this] { if (onDelete) onDelete(); };
    addAndMakeVisible(deleteButton);

    loadButton.setTooltip(isReadOnly ? "Load the previous session's autosave"
                                     : "Load this save into the live rig (replaces the current scene)");
    loadButton.onClick = [this] { if (onLoad) onLoad(); };
    addAndMakeVisible(loadButton);
    for (auto* button : {&defButton, &renameButton, &deleteButton, &loadButton})
        theme::ui::styleButton(*button);
    deleteButton.setColour(juce::TextButton::textColourOffId, theme::red());

    if (isReadOnly) {
        // The autosave file is rewritten on exit, so it can be loaded but not
        // renamed, deleted or made the startup default.
        defButton.setVisible(false);
        renameButton.setVisible(false);
        deleteButton.setVisible(false);
    }
    setWantsKeyboardFocus(true);
}

void SavesView::SaveRowComponent::setDefaultActive(bool isDefaultIn) {
    isDefault = isDefaultIn;
    defButton.setToggleState(isDefault, juce::dontSendNotification);
    repaint();
}

void SavesView::SaveRowComponent::setSelected(bool isSelectedIn) {
    if (isSelected == isSelectedIn) return;
    isSelected = isSelectedIn;
    repaint();
}

void SavesView::SaveRowComponent::setLoadEnabled(bool enabled) {
    loadButton.setEnabled(enabled);
}

void SavesView::SaveRowComponent::setActionsEnabled(bool enabled) {
    defButton.setEnabled(enabled);
    renameButton.setEnabled(enabled);
    deleteButton.setEnabled(enabled);
}

void SavesView::SaveRowComponent::resized() {
    auto area = getLocalBounds().reduced(theme::ui::kSpace3, theme::ui::kRowGap);
    // Actions sit with the save they act on; the date follows that group as a
    // quiet column instead of being stranded at the far right of a wide row.
    const bool compact = area.getWidth() < 560;
    metaLabel.setVisible(!compact || isReadOnly);
    if (isReadOnly) {
        // Load only: the autosave cannot be renamed, deleted or made default.
        loadButton.setBounds(area.removeFromRight(60));
        area.removeFromRight(theme::ui::kRowGap);
        if (metaLabel.isVisible()) metaLabel.setBounds(area.removeFromRight(juce::jmin(220, area.getWidth() / 2)));
        nameLabel.setBounds(area);
        return;
    }
    const int nameW = compact ? juce::jmax(80, area.getWidth() / 2)
                              : juce::jmin(360, area.getWidth() / 3);
    nameLabel.setBounds(area.removeFromLeft(nameW));
    area.removeFromLeft(theme::ui::kSpace3);
    defButton.setBounds(area.removeFromLeft(50));
    area.removeFromLeft(theme::ui::kRowGap);
    loadButton.setBounds(area.removeFromLeft(60));
    area.removeFromLeft(theme::ui::kRowGap);
    renameButton.setBounds(area.removeFromLeft(60));
    area.removeFromLeft(theme::ui::kRowGap);
    deleteButton.setBounds(area.removeFromLeft(64));
    if (compact) return;
    area.removeFromLeft(theme::ui::kSectionGap);
    metaLabel.setBounds(area.removeFromLeft(220));
}

void SavesView::SaveRowComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::panel());
    g.setColour(theme::border());
    g.drawRect(getLocalBounds(), 1);
    if (isSelected) {
        g.setColour(theme::green());
        g.drawRect(getLocalBounds().reduced(1), 2);
    } else if (isDefault) {
        g.setColour(theme::amber());
        g.drawRect(getLocalBounds().reduced(1), 1);
    }
}

void SavesView::SaveRowComponent::mouseUp(const juce::MouseEvent& event) {
    if (!event.mouseWasClicked()) return;
    grabKeyboardFocus();
    if (onSelect) onSelect();
}

bool SavesView::SaveRowComponent::keyPressed(const juce::KeyPress& key) {
    // The selected row drives the same actions as its buttons, so a save can be
    // loaded or removed without aiming at each button in turn.
    if (key == juce::KeyPress::returnKey) {
        if (onLoad) onLoad();
        return true;
    }
    if (isReadOnly) return false;
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
        if (onDelete) onDelete();
        return true;
    }
    if (key == juce::KeyPress(juce::KeyPress::F2Key)) {
        if (onRename) onRename();
        return true;
    }
    return false;
}

SavesView::SavesView(std::function<juce::String(const pluginrack::Scene&, std::function<void(const juce::String&)>)>
                         onRestoreIn,
                   std::function<void(const juce::String&)> onLoadedIn,
                   std::function<void(const juce::String&, const juce::String&)>
                       onSaveIdentityChangedIn)
    : onRestore(std::move(onRestoreIn))
    , onLoaded(std::move(onLoadedIn))
    , onSaveIdentityChanged(std::move(onSaveIdentityChangedIn)) {
    addAndMakeVisible(header);

    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    rebuild();
}

SavesView::~SavesView() = default;

void SavesView::rebuild() {
    const auto names = SceneBridge::getSaveNames();
    const auto defaultName = SceneBridge::getDefaultSaveName();
    const auto lastFile = SceneBridge::getLastSceneFile();

    entries.clear();
    for (const auto& name : names)
        entries.push_back({name, SceneBridge::getSaveFile(name), false});
    if (lastFile.existsAsFile()) entries.push_back({"Autosave", lastFile, true});

    // Keep the selection on the same save by name: a rename, delete or a new
    // save shifts the row indices.
    int index = -1;
    for (int i = 0; i < (int)entries.size(); ++i)
        if (entries[(size_t)i].name == selectedName) {
            index = i;
            break;
        }
    if (index < 0) index = entries.empty() ? -1 : 0;
    selectedRow = index;
    selectedName = index >= 0 ? entries[(size_t)index].name : juce::String();

    rows.clear();
    content.removeAllChildren();

    for (int i = 0; i < (int)entries.size(); ++i) {
        const auto& entry = entries[(size_t)i];
        auto row = std::make_unique<SaveRowComponent>(entry.name, entry.file,
                                                      !entry.readOnly && entry.name == defaultName,
                                                      i == selectedRow, entry.readOnly);
        row->onSelect = [this, i] {
            selectedRow = i;
            selectedName = entries[(size_t)i].name;
            updateSelection();
        };
        row->onLoad = [this, i] { loadSave(entries[(size_t)i]); };
        row->onSetDefault = [this, name = entry.name] {
            // A real toggle: switching the active default off clears it, so the
            // next launch restores the last exit autosave instead of this save.
            if (SceneBridge::getDefaultSaveName().equalsIgnoreCase(name))
                SceneBridge::setDefaultSaveName({});
            else
                SceneBridge::setDefaultSaveName(name);
            rebuild();
        };
        row->onRename = [this, name = entry.name] { promptRename(name); };
        row->onDelete = [this, name = entry.name] { confirmDelete(name); };
        row->setLoadEnabled(!loading_.load());
        row->setActionsEnabled(!loading_.load());
        content.addAndMakeVisible(row.get());
        rows.push_back(std::move(row));
    }

    layoutRows();
    repaint();
}

void SavesView::layoutRows() {
    int width = viewport.getWidth();
    if (width <= 0) width = getWidth();
    width = juce::jmax(100, width);
    const int totalHeight = rows.empty() ? 0 : (int)rows.size() * (kRowHeight + kRowGap) - kRowGap;
    content.setSize(width, juce::jmax(totalHeight, viewport.getHeight()));
    int y = 0;
    for (auto& row : rows) {
        row->setBounds(0, y, width, kRowHeight);
        y += kRowHeight + kRowGap;
    }
}

void SavesView::updateSelection() {
    for (int i = 0; i < (int)rows.size(); ++i) rows[(size_t)i]->setSelected(i == selectedRow);
}

void SavesView::setLoading(bool isLoading) {
    header.setStatus(isLoading ? "Loading..." : juce::String());
    for (auto& row : rows) {
        row->setLoadEnabled(!isLoading);
        row->setActionsEnabled(!isLoading);
    }
}

void SavesView::loadSave(const Entry& entry) {
    // The host starts a staged restore: it returns an immediate error, or runs
    // the completion once the engine has committed the new strips. loading_
    // keeps the rows disabled until then.
    if (loading_.exchange(true)) return;
    juce::String error;
    const auto saved = entry.readOnly ? SceneBridge::load(entry.file, error)
                                      : SceneBridge::loadNamed(entry.name, error);
    if (error.isNotEmpty()) {
        loading_.store(false);
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", error);
        return;
    }
    if (!saved.has_value()) {
        loading_.store(false);
        return;
    }
    setLoading(true);
    juce::Component::SafePointer<SavesView> safe(this);
    // The autosave is not a named save, so the host clears its active save
    // name: the toolbar Save asks for a name instead of overwriting the
    // autosave on exit.
    const auto loadedName = entry.readOnly ? juce::String() : entry.name;
    auto onFinished = [safe, loadedName](const juce::String& pluginErrors) {
        if (safe == nullptr) return;
        safe->loading_.store(false);
        safe->setLoading(false);
        if (pluginErrors.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack",
                "The scene was loaded, but some plugins could not be restored:\n" + pluginErrors);
        else if (safe->onLoaded)
            safe->onLoaded(loadedName);
    };
    if (!onRestore) {
        onFinished({});
        return;
    }
    const auto startError = onRestore(*saved, onFinished);
    if (startError.isNotEmpty()) {
        loading_.store(false);
        setLoading(false);
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack",
                                               startError);
    }
}

void SavesView::promptRename(const juce::String& oldName) {
    auto* w = new juce::AlertWindow("Rename Save", "New name for '" + oldName + "'.", juce::MessageBoxIconType::QuestionIcon);
    w->addTextEditor("name", oldName, "New name");
    w->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<SavesView> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w, oldName](int result) {
        if (result == 1 && safe != nullptr) {
            const auto newName = w->getTextEditorContents("name");
            const auto error = SceneBridge::renameSave(oldName, newName);
            if (error.isNotEmpty()) {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", error);
            } else {
                if (safe->onSaveIdentityChanged)
                    safe->onSaveIdentityChanged(
                        oldName, SceneBridge::getSaveFile(newName).getFileNameWithoutExtension());
                safe->rebuild();
            }
        }
        delete w;
    }), true);
}

void SavesView::confirmDelete(const juce::String& name) {
    juce::Component::SafePointer<SavesView> safe(this);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "PluginRack",
        "Delete save '" + name + "'?", "Delete", "Cancel", nullptr,
        juce::ModalCallbackFunction::create([safe, name](int result) {
            if (result == 1 && safe != nullptr) {
                if (SceneBridge::deleteSave(name) && safe->onSaveIdentityChanged)
                    safe->onSaveIdentityChanged(name, {});
                safe->rebuild();
            }
        }));
}

void SavesView::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
    if (!rows.empty()) return;
    if (!SceneBridge::getSaveNames().isEmpty()) return;  // rows exist but not laid out yet
    g.setColour(theme::dim());
    g.setFont(theme::ui::itemFont());
    g.drawFittedText("No saves yet. Use Save Scene As... in the Menu.",
                     viewport.getBounds().reduced(theme::ui::kSpace3), juce::Justification::centred, 2);
}

void SavesView::resized() {
    auto area = getLocalBounds().reduced(theme::ui::kPagePad);
    header.setBounds(area.removeFromTop(theme::ui::kHeaderH));
    area.removeFromTop(theme::ui::kHeaderGap);
    viewport.setBounds(area);
    layoutRows();
}
