#include "ChannelStrip.h"

ChannelStrip::InsertRow::InsertRow(const AudioEngine::InsertSlot& insert, ChannelStrip& owner) {
    nameLabel.setText(insert.pluginName.isNotEmpty()
                          ? insert.pluginName
                          : juce::File(insert.pluginPath).getFileName(),
                      juce::dontSendNotification);
    nameLabel.setTooltip(insert.pluginPath);
    nameLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel);

    bypassButton.setClickingTogglesState(true);
    bypassButton.setToggleState(insert.bypassed.get(), juce::dontSendNotification);
    bypassButton.onClick = [this, &owner, channelId = owner.channel.id, insertId = insert.id] {
        owner.audioEngine.setPluginBypassed(channelId, insertId, bypassButton.getToggleState());
    };
    addAndMakeVisible(bypassButton);

    removeButton.setTooltip("Remove plugin");
    removeButton.onClick = [this, &owner, channelId = owner.channel.id, insertId = insert.id] {
        owner.audioEngine.removePlugin(channelId, insertId);
        juce::Component::SafePointer<ChannelStrip> safeOwner(&owner);
        juce::MessageManager::callAsync([safeOwner] {
            if (safeOwner != nullptr) {
                safeOwner->refreshInserts();
            }
        });
    };
    addAndMakeVisible(removeButton);
}

void ChannelStrip::InsertRow::resized() {
    auto area = getLocalBounds();
    removeButton.setBounds(area.removeFromRight(28));
    area.removeFromRight(2);
    bypassButton.setBounds(area.removeFromRight(60));
    nameLabel.setBounds(area);
}

ChannelStrip::ChannelStrip(AudioEngine::Channel& channelState, AudioEngine& engine)
    : channel(channelState),
      audioEngine(engine),
      levelMeter(channelState.peak) {
    nameLabel.setText(channel.name, juce::dontSendNotification);
    nameLabel.setJustificationType(juce::Justification::centred);
    nameLabel.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    addAndMakeVisible(nameLabel);

    gainLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(gainLabel);
    gainSlider.setSliderStyle(juce::Slider::LinearVertical);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 20);
    gainSlider.setRange(-60.0, 12.0, 0.1);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setDoubleClickReturnValue(true, 0.0);
    gainSlider.setValue(channel.gainDb.get(), juce::dontSendNotification);
    gainSlider.onValueChange = [this] {
        audioEngine.setChannelGain(channel.id, static_cast<float>(gainSlider.getValue()));
    };
    addAndMakeVisible(gainSlider);

    panLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(panLabel);
    panSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    panSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 20);
    panSlider.setRange(-1.0, 1.0, 0.01);
    panSlider.setDoubleClickReturnValue(true, 0.0);
    panSlider.setValue(channel.pan.get(), juce::dontSendNotification);
    panSlider.onValueChange = [this] {
        audioEngine.setChannelPan(channel.id, static_cast<float>(panSlider.getValue()));
    };
    addAndMakeVisible(panSlider);

    for (auto* button : {&muteButton, &soloButton}) {
        button->setClickingTogglesState(true);
        addAndMakeVisible(*button);
    }
    muteButton.setToggleState(channel.muted.get(), juce::dontSendNotification);
    muteButton.onClick = [this] {
        audioEngine.setChannelMuted(channel.id, muteButton.getToggleState());
    };
    soloButton.setToggleState(channel.soloed.get(), juce::dontSendNotification);
    soloButton.onClick = [this] {
        audioEngine.setChannelSoloed(channel.id, soloButton.getToggleState());
    };

    addAndMakeVisible(levelMeter);
    addPluginButton.onClick = [this] { choosePlugin(); };
    addAndMakeVisible(addPluginButton);
    insertViewport.setViewedComponent(&insertContent, false);
    insertViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(insertViewport);
    refreshInserts();
}

void ChannelStrip::resized() {
    auto area = getLocalBounds().reduced(8);
    nameLabel.setBounds(area.removeFromTop(26));
    gainLabel.setBounds(area.removeFromTop(20));
    auto gainArea = area.removeFromTop(160);
    levelMeter.setBounds(gainArea.removeFromRight(18));
    gainArea.removeFromRight(6);
    gainSlider.setBounds(gainArea);
    area.removeFromTop(4);
    panLabel.setBounds(area.removeFromTop(20));
    panSlider.setBounds(area.removeFromTop(52));
    area.removeFromTop(4);

    auto buttons = area.removeFromTop(28);
    muteButton.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2));
    buttons.removeFromLeft(4);
    soloButton.setBounds(buttons);
    area.removeFromTop(8);
    addPluginButton.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    insertViewport.setBounds(area);
    layoutInsertRows();
}

void ChannelStrip::refreshInserts() {
    insertRows.clear();
    for (const auto& insert : channel.inserts) {
        auto* row = insertRows.add(new InsertRow(*insert, *this));
        insertContent.addAndMakeVisible(*row);
    }
    insertContent.setSize(juce::jmax(1, insertViewport.getWidth() - insertViewport.getScrollBarThickness()),
                          juce::jmax(1, insertRows.size() * 30));
    layoutInsertRows();
    insertViewport.resized();
}

void ChannelStrip::choosePlugin() {
    fileChooser = std::make_unique<juce::FileChooser>(
        "Select a VST3 plugin", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.vst3");
    juce::Component::SafePointer<ChannelStrip> safeThis(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                             [safeThis](const juce::FileChooser& chooser) {
                                 const auto file = chooser.getResult();
                                 if (safeThis == nullptr || file == juce::File()) {
                                     return;
                                 }
                                 const auto error = safeThis->audioEngine.addPlugin(safeThis->channel.id, file);
                                 if (error.isNotEmpty()) {
                                     juce::AlertWindow::showMessageBoxAsync(
                                         juce::MessageBoxIconType::WarningIcon, "PluginRack", error);
                                 }
                                 safeThis->refreshInserts();
                             });
}

void ChannelStrip::layoutInsertRows() {
    const int width = juce::jmax(1, insertViewport.getWidth() - insertViewport.getScrollBarThickness());
    for (int index = 0; index < insertRows.size(); ++index) {
        insertRows[index]->setBounds(0, index * 30, width, 28);
    }
    insertContent.setSize(width, juce::jmax(1, insertRows.size() * 30));
}
