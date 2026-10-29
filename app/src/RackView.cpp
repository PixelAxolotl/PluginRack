#include "RackView.h"

namespace {
constexpr int kRowHeight = 150;
constexpr int kRowGap = 8;
constexpr int kHeaderWidth = 280;
constexpr int kModuleWidth = 200;
constexpr int kAddButtonWidth = 120;
constexpr int kModuleGap = 8;
} // namespace

RackView::ChainRow::ChainRow(AudioEngine& engine, AudioEngine::Channel& channelState)
    : audioEngine(engine), channel(&channelState) {
    setWantsKeyboardFocus(true);
    meter = std::make_unique<LevelMeter>(channel->peak);
    initHeader();
    refresh();
}

RackView::ChainRow::ChainRow(AudioEngine& engine, AudioEngine::Output& outputState)
    : audioEngine(engine), output(&outputState) {
    setWantsKeyboardFocus(true);
    meter = std::make_unique<LevelMeter>(output->peak);
    initHeader();
    refresh();
}

void RackView::ChainRow::initHeader() {
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    nameLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    nameLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(nameLabel);

    identityLabel.setJustificationType(juce::Justification::centredLeft);
    identityLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    identityLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(identityLabel);

    routingLabel.setJustificationType(juce::Justification::centredLeft);
    routingLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    routingLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(routingLabel);

    addAndMakeVisible(*meter);

    for (auto* b : {&muteButton, &soloButton}) {
        b->setClickingTogglesState(true);
        addAndMakeVisible(*b);
    }
    muteButton.setColour(juce::TextButton::buttonColourId, theme::control());
    muteButton.setColour(juce::TextButton::buttonOnColourId, theme::red());
    muteButton.setColour(juce::TextButton::textColourOffId, theme::text());
    muteButton.onClick = [this] {
        if (channel != nullptr) audioEngine.setChannelMuted(channel->id, muteButton.getToggleState());
        else audioEngine.setOutputMuted(output->id, muteButton.getToggleState());
        if (onSelected) onSelected(this);
    };
    soloButton.setColour(juce::TextButton::buttonColourId, theme::control());
    soloButton.setColour(juce::TextButton::buttonOnColourId, theme::amber());
    soloButton.setColour(juce::TextButton::textColourOffId, theme::text());
    soloButton.onClick = [this] {
        if (channel != nullptr) audioEngine.setChannelSoloed(channel->id, soloButton.getToggleState());
        if (onSelected) onSelected(this);
    };
    soloButton.setVisible(channel != nullptr);

    mixerButton.setColour(juce::TextButton::buttonColourId, theme::control());
    mixerButton.setColour(juce::TextButton::textColourOffId, theme::cyan());
    mixerButton.setTooltip("Show this chain in the Mixer view");
    mixerButton.onClick = [this] {
        if (onSelected) onSelected(this);
        if (onJumpToMixer) onJumpToMixer();
    };
    addAndMakeVisible(mixerButton);

    modeCombo.addItem("Mono", 1);
    modeCombo.addItem("Stereo", 2);
    modeCombo.setColour(juce::ComboBox::backgroundColourId, theme::control());
    modeCombo.setColour(juce::ComboBox::textColourId, theme::text());
    modeCombo.setColour(juce::ComboBox::arrowColourId, theme::dim());
    modeCombo.setColour(juce::ComboBox::outlineColourId, theme::border());
    modeCombo.onChange = [this] {
        if (output != nullptr) {
            audioEngine.setOutputLayout(output->id,
                modeCombo.getSelectedId() == 1 ? pluginrack::ChannelLayout::Mono
                                              : pluginrack::ChannelLayout::Stereo);
        }
    };
    addAndMakeVisible(modeCombo);
    modeCombo.setVisible(output != nullptr);

    flipButton.setClickingTogglesState(true);
    flipButton.setColour(juce::TextButton::buttonColourId, theme::control());
    flipButton.setColour(juce::TextButton::buttonOnColourId, theme::cyan());
    flipButton.setColour(juce::TextButton::textColourOffId, theme::text());
    flipButton.onClick = [this] {
        if (output != nullptr) audioEngine.setOutputFlipped(output->id, flipButton.getToggleState());
    };
    addAndMakeVisible(flipButton);
    flipButton.setVisible(output != nullptr);

    emptyLabel.setJustificationType(juce::Justification::centred);
    emptyLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    emptyLabel.setColour(juce::Label::textColourId, theme::dim());
    emptyLabel.setText("Empty — chain is clean", juce::dontSendNotification);
    moduleContent.addAndMakeVisible(emptyLabel);

    addVstButton.setColour(juce::TextButton::buttonColourId, theme::control());
    addVstButton.setColour(juce::TextButton::textColourOffId, theme::violet());
    addVstButton.onClick = [this] { openPicker(); };
    moduleContent.addAndMakeVisible(addVstButton);

    moduleViewport.setViewedComponent(&moduleContent, false);
    moduleViewport.setScrollBarsShown(false, true);
    addAndMakeVisible(moduleViewport);
}

void RackView::ChainRow::refresh() {
    if (channel != nullptr) {
        nameLabel.setText(channel->name, juce::dontSendNotification);
        juce::String identity = channel->inputDeviceName.isNotEmpty() ? channel->inputDeviceName
                                                                      : juce::String("No input");
        identity += channel->layout == pluginrack::ChannelLayout::Mono ? " - Mono" : " - Stereo";
        identityLabel.setText(identity, juce::dontSendNotification);
        routingLabel.setText(buildRoutingSummary(), juce::dontSendNotification);
        routingLabel.setColour(juce::Label::textColourId, theme::dim());
        muteButton.setToggleState(channel->muted.get(), juce::dontSendNotification);
        soloButton.setToggleState(channel->soloed.get(), juce::dontSendNotification);
    } else {
        nameLabel.setText(output->name, juce::dontSendNotification);
        identityLabel.setText(output->outputDeviceName.isNotEmpty() ? output->outputDeviceName
                                                                    : juce::String("No device"),
                              juce::dontSendNotification);
        if (output->outputError.isNotEmpty()) {
            routingLabel.setText(output->outputError, juce::dontSendNotification);
            routingLabel.setColour(juce::Label::textColourId, theme::red());
        } else {
            routingLabel.setText("Output bus", juce::dontSendNotification);
            routingLabel.setColour(juce::Label::textColourId, theme::dim());
        }
        muteButton.setToggleState(output->muted.get(), juce::dontSendNotification);
        modeCombo.setSelectedId(output->layout == pluginrack::ChannelLayout::Mono ? 1 : 2, juce::dontSendNotification);
        flipButton.setToggleState(output->flipped.get(), juce::dontSendNotification);
    }
    refreshModules();
}

void RackView::ChainRow::setSelected(bool isSelected) {
    if (selected == isSelected) return;
    selected = isSelected;
    repaint();
}

juce::String RackView::ChainRow::buildRoutingSummary() const {
    if (channel == nullptr) return {};
    juce::StringArray targets;
    for (const auto& out : audioEngine.getOutputs()) {
        if (audioEngine.getSendGain(channel->id, out->id) > -30.0f) targets.add(out->name);
    }
    if (targets.isEmpty()) return "No routes";
    return juce::String("-> ") + targets.joinIntoString(", ");
}

void RackView::ChainRow::refreshModules() {
    slots.clear();
    const auto& inserts = channel != nullptr ? channel->inserts : output->inserts;
    for (int i = 0; i < (int)inserts.size(); ++i) {
        auto* slot = slots.add(new InsertSlotComponent(*this, i));
        slot->setInsert(inserts[(std::size_t)i].get());
        moduleContent.addAndMakeVisible(*slot);
    }
    emptyLabel.setVisible(slots.isEmpty());
    layoutModules();
}

void RackView::ChainRow::layoutModules() {
    const int h = juce::jmax(0, moduleViewport.getHeight());
    const int innerH = juce::jmax(0, h - 8);
    int x = 4;
    for (auto* slot : slots) {
        slot->setBounds(x, 4, kModuleWidth, innerH);
        x += kModuleWidth + kModuleGap;
    }
    if (slots.isEmpty()) {
        emptyLabel.setBounds(x, 4, 220, innerH);
        x += 220 + kModuleGap;
    }
    addVstButton.setBounds(x, 4, kAddButtonWidth, innerH);
    x += kAddButtonWidth + 4;
    moduleContent.setSize(x, juce::jmax(0, h));
}

void RackView::ChainRow::resized() {
    auto area = getLocalBounds().reduced(6);
    auto header = area.removeFromLeft(kHeaderWidth);
    area.removeFromLeft(8);
    moduleViewport.setBounds(area);

    auto meterArea = header.removeFromRight(16);
    header.removeFromRight(6);
    if (meter != nullptr) meter->setBounds(meterArea);

    nameLabel.setBounds(header.removeFromTop(22));
    identityLabel.setBounds(header.removeFromTop(18));
    routingLabel.setBounds(header.removeFromTop(18));
    header.removeFromTop(4);

    if (modeCombo.isVisible()) {
        auto modeRow = header.removeFromTop(20);
        modeCombo.setBounds(modeRow.removeFromLeft(modeRow.getWidth() / 2 - 2));
        modeRow.removeFromLeft(4);
        flipButton.setBounds(modeRow);
        header.removeFromTop(2);
    }

    auto buttonRow = header.removeFromBottom(24);
    if (soloButton.isVisible()) {
        const int third = buttonRow.getWidth() / 3 - 3;
        muteButton.setBounds(buttonRow.removeFromLeft(third));
        buttonRow.removeFromLeft(4);
        soloButton.setBounds(buttonRow.removeFromLeft(third));
        buttonRow.removeFromLeft(4);
        mixerButton.setBounds(buttonRow);
    } else {
        const int half = buttonRow.getWidth() / 2 - 2;
        muteButton.setBounds(buttonRow.removeFromLeft(half));
        buttonRow.removeFromLeft(4);
        mixerButton.setBounds(buttonRow);
    }

    layoutModules();
}

void RackView::ChainRow::paint(juce::Graphics& g) {
    g.fillAll(theme::strip());
    if (selected) {
        g.setColour(theme::green());
        g.fillRect(0, 0, 4, getHeight());
        g.drawRect(getLocalBounds(), 2);
    } else {
        g.setColour(theme::border());
        g.drawRect(getLocalBounds(), 1);
    }
    if (hasKeyboardFocus(true)) {
        g.setColour(theme::cyan());
        g.drawRect(getLocalBounds().reduced(1), 1);
    }
}

void RackView::ChainRow::mouseDown(const juce::MouseEvent&) {
    grabKeyboardFocus();
    if (onSelected) onSelected(this);
}

void RackView::ChainRow::mouseDoubleClick(const juce::MouseEvent&) {
    if (onSelected) onSelected(this);
    if (onJumpToMixer) onJumpToMixer();
}

bool RackView::ChainRow::keyPressed(const juce::KeyPress& key) {
    if (key.isKeyCode(juce::KeyPress::upKey)) {
        if (onMoveSelection) onMoveSelection(-1);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::downKey)) {
        if (onMoveSelection) onMoveSelection(1);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::returnKey)) {
        if (onSelected) onSelected(this);
        return true;
    }
    return false;
}

juce::String RackView::ChainRow::insertPlugin(const juce::File& file) {
    const auto error = channel != nullptr ? audioEngine.addPlugin(channel->id, file)
                                          : audioEngine.addOutputPlugin(output->id, file);
    refreshAfterInsertChange();
    return error;
}

void RackView::ChainRow::erasePlugin(std::uint32_t insertId) {
    if (channel != nullptr) audioEngine.removePlugin(channel->id, insertId);
    else audioEngine.removeOutputPlugin(output->id, insertId);
}

void RackView::ChainRow::setPluginBypass(std::uint32_t insertId, bool bypassed) {
    if (channel != nullptr) audioEngine.setPluginBypassed(channel->id, insertId, bypassed);
    else audioEngine.setOutputPluginBypassed(output->id, insertId, bypassed);
}

void RackView::ChainRow::refreshAfterInsertChange() {
    refreshModules();
    juce::Component::SafePointer<RackView::ChainRow> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        auto callback = safe->onStructureChanged;
        if (callback) callback();
    });
}

void RackView::ChainRow::openPicker() {
    fileChooser = std::make_unique<juce::FileChooser>(
        "Select a VST3 plugin", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.vst3");
    juce::Component::SafePointer<ChainRow> safeThis(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                             [safeThis](const juce::FileChooser& chooser) {
                                 const auto file = chooser.getResult();
                                 if (safeThis == nullptr || file == juce::File()) return;
                                 const auto error = safeThis->insertPlugin(file);
                                 if (error.isNotEmpty()) {
                                     juce::AlertWindow::showMessageBoxAsync(
                                         juce::MessageBoxIconType::WarningIcon, "PluginRack", error);
                                 }
                             });
}

RackView::RackView(AudioEngine& engine) : audioEngine(engine) {
    setWantsKeyboardFocus(true);
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    emptyLabel.setText("No chains yet — add a channel or output in the Mixer view.",
                       juce::dontSendNotification);
    emptyLabel.setJustificationType(juce::Justification::centred);
    emptyLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    emptyLabel.setColour(juce::Label::textColourId, theme::dim());
    emptyLabel.setVisible(false);
    addAndMakeVisible(emptyLabel);

    listViewport.setViewedComponent(&listContent, false);
    listViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(listViewport);

    rebuild();
}

void RackView::rebuild() {
    rows.clear();
    for (const auto& ch : audioEngine.getChannels()) {
        auto* row = rows.add(new ChainRow(audioEngine, *ch));
        row->onSelected = [this](ChainRow* selected) { selectRow(selected); };
        row->onMoveSelection = [this](int delta) { selectRelative(delta); };
        row->onJumpToMixer = [this] { jumpToMixer(); };
        row->onChanged = [this] { rebuild(); };
        row->onStructureChanged = [this] { if (onStructureChanged) onStructureChanged(); };
        if (hasSelection && !selectedIsOutput && selectedId == ch->id) row->setSelected(true);
        listContent.addAndMakeVisible(*row);
    }
    for (const auto& out : audioEngine.getOutputs()) {
        auto* row = rows.add(new ChainRow(audioEngine, *out));
        row->onSelected = [this](ChainRow* selected) { selectRow(selected); };
        row->onMoveSelection = [this](int delta) { selectRelative(delta); };
        row->onJumpToMixer = [this] { jumpToMixer(); };
        row->onChanged = [this] { rebuild(); };
        row->onStructureChanged = [this] { if (onStructureChanged) onStructureChanged(); };
        if (hasSelection && selectedIsOutput && selectedId == out->id) row->setSelected(true);
        listContent.addAndMakeVisible(*row);
    }
    emptyLabel.setVisible(rows.isEmpty());
    listViewport.setVisible(!rows.isEmpty());
    resized();
}

void RackView::selectRow(ChainRow* row) {
    if (row == nullptr) return;
    hasSelection = true;
    selectedIsOutput = row->isOutput();
    selectedId = row->chainId();
    for (auto* r : rows) r->setSelected(r == row);
    row->grabKeyboardFocus();
}

void RackView::selectRelative(int delta) {
    if (rows.isEmpty()) return;
    int index = 0;
    if (hasSelection) {
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i]->chainId() == selectedId && rows[i]->isOutput() == selectedIsOutput) {
                index = i;
                break;
            }
        }
        index = juce::jlimit(0, rows.size() - 1, index + delta);
    }
    selectRow(rows[index]);
}

void RackView::jumpToMixer() {
    if (onJumpToMixer) onJumpToMixer();
}

bool RackView::keyPressed(const juce::KeyPress& key) {
    if (key.isKeyCode(juce::KeyPress::upKey)) {
        selectRelative(-1);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::downKey)) {
        selectRelative(1);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::returnKey)) {
        if (hasSelection) selectRelative(0);
        return true;
    }
    return false;
}

void RackView::resized() {
    auto area = getLocalBounds();
    if (emptyLabel.isVisible()) emptyLabel.setBounds(area.reduced(24));
    listViewport.setBounds(area);

    const int w = juce::jmax(0, listViewport.getWidth());
    int y = 4;
    for (auto* row : rows) {
        row->setBounds(4, y, juce::jmax(0, w - 8), kRowHeight);
        y += kRowHeight + kRowGap;
    }
    listContent.setSize(w, y);
}
