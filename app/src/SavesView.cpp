#include "SavesView.h"

#include "SceneBridge.h"
#include "Theme.h"

SavesView::SaveRowComponent::SaveRowComponent(const juce::String& name, bool isDefaultIn, bool isSelectedIn)
    : saveName(name)
    , isDefault(isDefaultIn)
    , isSelected(isSelectedIn) {
    nameLabel.setText(saveName, juce::dontSendNotification);
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    nameLabel.setFont(theme::ui::itemFont());
    nameLabel.setColour(juce::Label::textColourId, theme::text());
    nameLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel);

    defButton.setClickingTogglesState(true);
    defButton.setToggleState(isDefault, juce::dontSendNotification);
    defButton.setTooltip("Set as default save");
    defButton.onClick = [this] { if (onSetDefault) onSetDefault(); };
    addAndMakeVisible(defButton);

    renameButton.setTooltip("Rename this save");
    renameButton.onClick = [this] { if (onRename) onRename(); };
    addAndMakeVisible(renameButton);

    deleteButton.setTooltip("Delete this save");
    deleteButton.onClick = [this] { if (onDelete) onDelete(); };
    addAndMakeVisible(deleteButton);

    loadButton.setTooltip("Load this save");
    loadButton.onClick = [this] { if (onLoad) onLoad(); };
    addAndMakeVisible(loadButton);
    for (auto* button : {&defButton, &renameButton, &deleteButton, &loadButton})
        theme::ui::styleButton(*button);
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

void SavesView::SaveRowComponent::resized() {
    auto area = getLocalBounds().reduced(12, 8);
    deleteButton.setBounds(area.removeFromRight(70));
    area.removeFromRight(8);
    renameButton.setBounds(area.removeFromRight(70));
    area.removeFromRight(8);
    loadButton.setBounds(area.removeFromRight(60));
    area.removeFromRight(8);
    defButton.setBounds(area.removeFromRight(60));
    area.removeFromRight(8);
    nameLabel.setBounds(area);
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
    if (event.mouseWasClicked() && onSelect) onSelect();
}

SavesView::SavesView(std::function<juce::String(const pluginrack::Scene&)> onRestoreIn,
                   std::function<void(const juce::String&)> onLoadedIn)
    : onRestore(std::move(onRestoreIn)), onLoaded(std::move(onLoadedIn)) {
    titleLabel.setText("Saves", juce::dontSendNotification);
    titleLabel.setFont(theme::ui::titleFont());
    titleLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(titleLabel);

    statusLabel.setJustificationType(juce::Justification::centredRight);
    statusLabel.setFont(theme::ui::captionFont());
    statusLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(statusLabel);

    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    rebuild();
}

SavesView::~SavesView() = default;

void SavesView::rebuild() {
    const auto names = SceneBridge::getSaveNames();
    const auto defaultName = SceneBridge::getDefaultSaveName();

    if (selectedRow >= names.size()) selectedRow = names.isEmpty() ? -1 : 0;
    if (selectedRow < 0 && !names.isEmpty()) selectedRow = 0;

    rows.clear();
    content.removeAllChildren();

    for (int i = 0; i < names.size(); ++i) {
        const auto& name = names[i];
        auto row = std::make_unique<SaveRowComponent>(name, name == defaultName, i == selectedRow);
        row->onSelect = [this, i] {
            selectedRow = i;
            updateSelection();
        };
        row->onLoad = [this, name] { loadSave(name); };
        row->onSetDefault = [this, name] {
            SceneBridge::setDefaultSaveName(name);
            rebuild();
        };
        row->onRename = [this, name] { promptRename(name); };
        row->onDelete = [this, name] { confirmDelete(name); };
        row->setLoadEnabled(!loading_.load());
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
    statusLabel.setText(isLoading ? "Loading..." : juce::String(), juce::dontSendNotification);
    for (auto& row : rows) row->setLoadEnabled(!isLoading);
}

void SavesView::loadSave(const juce::String& name) {
    // restoreScene instantiates plugins and destroys JUCE components with
    // its old strips, so it must run on the message thread. The host's
    // callback applies the parsed scene there (including its trust gate);
    // the engine parks the audio clock around the strip swap, which keeps
    // the synchronous load brief. loading_ serialises row clicks and resets
    // on every exit path.
    if (loading_.exchange(true)) return;
    juce::String error;
    const auto saved = SceneBridge::loadNamed(name, error);
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
    const juce::String pluginErrors = onRestore ? onRestore(*saved) : juce::String();
    loading_.store(false);
    setLoading(false);
    if (pluginErrors.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack",
            "The scene was loaded, but some plugins could not be restored:\n" + pluginErrors);
    else if (onLoaded)
        onLoaded(name);
}

void SavesView::promptRename(const juce::String& oldName) {
    auto* w = new juce::AlertWindow("Rename Save", "New name for '" + oldName + "'.", juce::MessageBoxIconType::QuestionIcon);
    w->addTextEditor("name", oldName, "New name");
    w->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<SavesView> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w, oldName](int result) {
        if (result == 1 && safe != nullptr) {
            const auto error = SceneBridge::renameSave(oldName, w->getTextEditorContents("name"));
            if (error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", error);
            else
                safe->rebuild();
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
                (void)SceneBridge::deleteSave(name);
                safe->rebuild();
            }
        }));
}

void SavesView::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
    g.setColour(theme::border().withAlpha(0.8f));
    g.drawHorizontalLine(theme::ui::kPagePad + theme::ui::kHeaderH - 1,
                         (float)theme::ui::kPagePad,
                         (float)(getWidth() - theme::ui::kPagePad));
}

void SavesView::resized() {
    auto area = getLocalBounds().reduced(theme::ui::kPagePad);
    auto top = area.removeFromTop(theme::ui::kHeaderH);
    statusLabel.setBounds(top.removeFromRight(200));
    titleLabel.setBounds(top);
    area.removeFromTop(theme::ui::kHeaderGap);
    viewport.setBounds(area);
    layoutRows();
}
