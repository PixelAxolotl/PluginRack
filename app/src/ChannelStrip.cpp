#include "ChannelStrip.h"

#include <algorithm>

namespace {
constexpr int kStripWidth = 148;
}

ChannelStrip::InsertSlotComponent::InsertSlotComponent(ChannelStrip& ownerRef, int index)
    : owner(ownerRef), slotIndex(index) {
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
        owner.audioEngine.setPluginBypassed(owner.channel.id, insert->id, next);
    });
    bypassButton.setTooltip("Bypass this plugin");
    addAndMakeVisible(bypassButton);

    removeButton.setClickAction([this] {
        if (insert == nullptr) return;
        owner.audioEngine.removePlugin(owner.channel.id, insert->id);
        juce::Component::SafePointer<ChannelStrip> safeOwner(&owner);
        juce::MessageManager::callAsync([safeOwner] {
            if (safeOwner != nullptr) safeOwner->refreshInserts();
        });
    });
    removeButton.setTooltip("Remove this plugin");
    addAndMakeVisible(removeButton);
}

void ChannelStrip::InsertSlotComponent::setInsert(AudioEngine::InsertSlot* newInsert) {
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

void ChannelStrip::InsertSlotComponent::resized() {
    auto area = getLocalBounds().reduced(3, 1);
    indexLabel.setBounds(area.removeFromLeft(14));
    removeButton.setBounds(area.removeFromRight(18));
    area.removeFromRight(2);
    bypassButton.setBounds(area.removeFromRight(18));
    area.removeFromRight(3);
    nameLabel.setBounds(area);
}

void ChannelStrip::InsertSlotComponent::mouseDown(const juce::MouseEvent&) {
    if (insert == nullptr) {
        owner.choosePlugin();
    } else {
        insert->pluginHost.showEditor();
    }
}

void ChannelStrip::InsertSlotComponent::mouseEnter(const juce::MouseEvent&) {
    hovered = true;
    repaint();
}

void ChannelStrip::InsertSlotComponent::mouseExit(const juce::MouseEvent&) {
    hovered = false;
    repaint();
}

void ChannelStrip::InsertSlotComponent::paint(juce::Graphics& g) {
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

ChannelStrip::ChannelStrip(AudioEngine::Channel& channelState, AudioEngine& engine)
    : channel(channelState),
      audioEngine(engine),
      levelMeter(channelState.peak) {
    deviceControl.setTitle(channel.name);
    deviceControl.setDeviceName(channel.inputDeviceName);
    deviceControl.setError(channel.inputError);

    channelLabel.setText("CH " + juce::String(channel.id), juce::dontSendNotification);
    channelLabel.setJustificationType(juce::Justification::centredLeft);
    channelLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    channelLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(channelLabel);

    removeChannelButton.setFontSize(13.0f);
    removeChannelButton.setClickAction([this] {
        juce::Component::SafePointer<ChannelStrip> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe == nullptr) return;
            // Copy the callback: the strip is destroyed by the handler that runs it.
            auto callback = safe->onRemoveRequested;
            if (callback) callback();
        });
    });
    removeChannelButton.setTooltip("Remove this channel");
    addAndMakeVisible(removeChannelButton);
    deviceControl.setOnClick([this] {
        DeviceMenu::showInputMenu(deviceControl, channel.inputDeviceName,
                                  audioEngine.getAvailableInputDevices(),
                                  [this](juce::String chosen) {
            const auto err = audioEngine.setChannelInputDevice(channel.id, chosen);
            deviceControl.setDeviceName(channel.inputDeviceName);
            deviceControl.setError(channel.inputError);
            if (err.isNotEmpty()) {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
            }
        });
    });
    addAndMakeVisible(deviceControl);

    vstHeader.setJustificationType(juce::Justification::centredLeft);
    vstHeader.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    vstHeader.setColour(juce::Label::textColourId, theme::violet());
    addAndMakeVisible(vstHeader);

    for (int i = 0; i < 4; ++i) {
        auto* slot = slots.add(new InsertSlotComponent(*this, i));
        addAndMakeVisible(*slot);
    }

    addVstButton.setColour(juce::TextButton::buttonColourId, theme::control());
    addVstButton.setColour(juce::TextButton::textColourOffId, theme::violet());
    addVstButton.onClick = [this] { choosePlugin(); };
    addAndMakeVisible(addVstButton);

    levelLabel.setJustificationType(juce::Justification::centred);
    levelLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    levelLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(levelLabel);

    levelKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    levelKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 48, 16);
    levelKnob.setRange(-12.0, 12.0, 0.1);
    levelKnob.setTextValueSuffix(" dB");
    levelKnob.setDoubleClickReturnValue(true, 0.0);
    levelKnob.setValue(channel.trimDb.get(), juce::dontSendNotification);
    levelKnob.onValueChange = [this] {
        audioEngine.setChannelTrim(channel.id, static_cast<float>(levelKnob.getValue()));
    };
    levelKnob.setColour(juce::Slider::rotarySliderFillColourId, theme::green());
    levelKnob.setColour(juce::Slider::rotarySliderOutlineColourId, theme::deepGreen());
    levelKnob.setColour(juce::Slider::textBoxTextColourId, theme::text());
    levelKnob.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    addAndMakeVisible(levelKnob);

    panLabel.setJustificationType(juce::Justification::centred);
    panLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    panLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(panLabel);

    panKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    panKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 48, 16);
    panKnob.setRange(-1.0, 1.0, 0.01);
    panKnob.setDoubleClickReturnValue(true, 0.0);
    panKnob.setValue(channel.pan.get(), juce::dontSendNotification);
    panKnob.onValueChange = [this] {
        audioEngine.setChannelPan(channel.id, static_cast<float>(panKnob.getValue()));
    };
    panKnob.setColour(juce::Slider::rotarySliderFillColourId, theme::cyan());
    panKnob.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
    panKnob.setColour(juce::Slider::textBoxTextColourId, theme::text());
    panKnob.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    addAndMakeVisible(panKnob);

    rebuildRoutingButtons();

    faderSlider.setSliderStyle(juce::Slider::LinearVertical);
    faderSlider.setTextBoxStyle(juce::Slider::TextBoxAbove, false, 44, 16);
    faderSlider.setRange(-60.0, 12.0, 0.1);
    faderSlider.setTextValueSuffix(" dB");
    faderSlider.setDoubleClickReturnValue(true, 0.0);
    faderSlider.setValue(channel.gainDb.get(), juce::dontSendNotification);
    faderSlider.onValueChange = [this] {
        audioEngine.setChannelGain(channel.id, static_cast<float>(faderSlider.getValue()));
    };
    faderSlider.setColour(juce::Slider::trackColourId, theme::deepGreen());
    faderSlider.setColour(juce::Slider::backgroundColourId, theme::strip());
    faderSlider.setColour(juce::Slider::thumbColourId, theme::green());
    faderSlider.setColour(juce::Slider::textBoxTextColourId, theme::text());
    faderSlider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    addAndMakeVisible(faderSlider);

    addAndMakeVisible(levelMeter);

    for (auto* b : {&muteButton, &soloButton}) {
        b->setClickingTogglesState(true);
        addAndMakeVisible(*b);
    }
    muteButton.setToggleState(channel.muted.get(), juce::dontSendNotification);
    muteButton.setColour(juce::TextButton::buttonColourId, theme::control());
    muteButton.setColour(juce::TextButton::buttonOnColourId, theme::red());
    muteButton.setColour(juce::TextButton::textColourOffId, theme::text());
    muteButton.onClick = [this] {
        audioEngine.setChannelMuted(channel.id, muteButton.getToggleState());
    };
    soloButton.setToggleState(channel.soloed.get(), juce::dontSendNotification);
    soloButton.setColour(juce::TextButton::buttonColourId, theme::control());
    soloButton.setColour(juce::TextButton::buttonOnColourId, theme::amber());
    soloButton.setColour(juce::TextButton::textColourOffId, theme::text());
    soloButton.onClick = [this] {
        audioEngine.setChannelSoloed(channel.id, soloButton.getToggleState());
    };

    refreshInserts();
    refreshRouting();
}

void ChannelStrip::resized() {
    auto area = getLocalBounds().reduced(4);

    // header row
    auto headerRow = area.removeFromTop(22);
    auto btnArea = headerRow.removeFromRight(22);
    channelLabel.setBounds(headerRow);
    removeChannelButton.setBounds(btnArea);
    area.removeFromTop(4);

    deviceControl.setBounds(area.removeFromTop(40));
    area.removeFromTop(4);

    vstHeader.setBounds(area.removeFromTop(14));
    // slots
    const int slotH = 22;
    for (int i = 0; i < slots.size(); ++i) {
        slots[i]->setBounds(area.removeFromTop(slotH));
        area.removeFromTop(2);
    }
    addVstButton.setBounds(area.removeFromTop(20));
    area.removeFromTop(6);

    // knobs row
    auto knobRow = area.removeFromTop(88);
    auto leftKnob = knobRow.removeFromLeft((knobRow.getWidth() - 8) / 2);
    levelLabel.setBounds(leftKnob.removeFromTop(14));
    levelKnob.setBounds(leftKnob.removeFromTop(72));
    knobRow.removeFromLeft(8);
    panLabel.setBounds(knobRow.removeFromTop(14));
    panKnob.setBounds(knobRow.removeFromTop(72));

    area.removeFromTop(4);
    // routing buttons grid
    const int rows = juce::jmax(1, ((int)routingButtons.size() + 3) / 4);
    const int routingHeight = rows * 22;
    auto routingArea = area.removeFromTop(routingHeight);
    const int cellW = juce::jmax(1, routingArea.getWidth() / 4);
    for (int i = 0; i < (int)routingButtons.size(); ++i) {
        const int row = i / 4;
        const int col = i % 4;
        routingButtons[i]->setBounds(routingArea.getX() + col * cellW + 1,
                                    routingArea.getY() + row * 22,
                                    juce::jmax(1, cellW - 2), 20);
    }
    area.removeFromTop(4);

    // fader + meter
    auto bottom = area.removeFromBottom(30); // mute/solo
    auto fm = area; // remaining for fader+meter
    if (fm.getHeight() < 140) {
        // keep minimum; area already consumed? leave as is
    }
    const int meterW = 12;
    levelMeter.setBounds(fm.removeFromRight(meterW));
    fm.removeFromRight(4);
    faderSlider.setBounds(fm);

    muteButton.setBounds(bottom.removeFromLeft(bottom.getWidth() / 2));
    bottom.removeFromLeft(4);
    soloButton.setBounds(bottom);
}

void ChannelStrip::refreshInserts() {
    // update slots
    for (int i = 0; i < slots.size(); ++i) {
        AudioEngine::InsertSlot* ins = nullptr;
        if (i < (int)channel.inserts.size()) ins = channel.inserts[i].get();
        slots[i]->setInsert(ins);
    }
    repaint();
}

void ChannelStrip::refreshRouting() {
    const auto& outputs = audioEngine.getOutputs();
    for (int i = 0; i < (int)routingButtons.size(); ++i) {
        bool on = false;
        bool enabled = false;
        if (i < (int)routingOutputIds.size() && i < (int)outputs.size()) {
            const float g = audioEngine.getSendGain(channel.id, routingOutputIds[i]);
            on = g > -30.0f;
            enabled = true;
        }
        routingButtons[i]->setEnabled(enabled);
        routingButtons[i]->setToggleState(on, juce::dontSendNotification);
    }
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
                                 if (safeThis == nullptr || file == juce::File()) return;
                                 const auto error = safeThis->audioEngine.addPlugin(safeThis->channel.id, file);
                                 if (error.isNotEmpty()) {
                                     juce::AlertWindow::showMessageBoxAsync(
                                         juce::MessageBoxIconType::WarningIcon, "PluginRack", error);
                                 }
                                 safeThis->refreshInserts();
                             });
}

void ChannelStrip::rebuildRoutingButtons() {
    for (auto* btn : routingButtons) removeChildComponent(btn);
    routingButtons.clear();
    routingOutputIds.clear();

    for (const auto& output : audioEngine.getOutputs()) {
        juce::String label = output->name;
        if (label.startsWithIgnoreCase("Output ")) {
            label = label.substring(7).trim();
        }

        auto* btn = routingButtons.add(new juce::TextButton(label));
        btn->setClickingTogglesState(true);
        btn->setColour(juce::TextButton::buttonColourId, theme::control());
        btn->setColour(juce::TextButton::buttonOnColourId, theme::green());
        btn->setColour(juce::TextButton::textColourOffId, theme::text());
        btn->setColour(juce::TextButton::textColourOnId, juce::Colour(0xff1b2229));

        const std::uint32_t outputId = output->id;
        routingOutputIds.push_back(outputId);
        btn->onClick = [this, btn, outputId] {
            audioEngine.setSendGain(channel.id, outputId, btn->getToggleState() ? 0.0f : -60.0f);
        };
        addAndMakeVisible(*btn);
    }

    refreshRouting();
}

void ChannelStrip::paint(juce::Graphics& g) {
    g.fillAll(theme::strip());
    // border
    g.setColour(theme::border());
    g.drawRect(getLocalBounds(), 1);
}
