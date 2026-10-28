#include "ChannelStrip.h"

#include <algorithm>

namespace {
constexpr int kStripWidth = 148;
}

ChannelStrip::InsertSlotComponent::InsertSlotComponent(ChannelStrip& ownerRef, int index)
    : owner(ownerRef), slotIndex(index) {
    indexLabel.setText(juce::String(index + 1), juce::dontSendNotification);
    indexLabel.setJustificationType(juce::Justification::centred);
    indexLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(indexLabel);

    nameLabel.setJustificationType(juce::Justification::centredLeft);
    nameLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel);

    bypassButton.setClickingTogglesState(true);
    bypassButton.setColour(juce::TextButton::buttonColourId, theme::control());
    bypassButton.setColour(juce::TextButton::buttonOnColourId, theme::violet());
    bypassButton.onClick = [this] {
        if (insert == nullptr) return;
        owner.audioEngine.setPluginBypassed(owner.channel.id, insert->id, bypassButton.getToggleState());
    };
    addAndMakeVisible(bypassButton);

    removeButton.onClick = [this] {
        if (insert == nullptr) return;
        owner.audioEngine.removePlugin(owner.channel.id, insert->id);
        juce::Component::SafePointer<ChannelStrip> safeOwner(&owner);
        juce::MessageManager::callAsync([safeOwner] {
            if (safeOwner != nullptr) safeOwner->refreshInserts();
        });
    };
    addAndMakeVisible(removeButton);
}

void ChannelStrip::InsertSlotComponent::setInsert(const AudioEngine::InsertSlot* newInsert) {
    insert = newInsert;
    if (insert != nullptr) {
        nameLabel.setText(insert->pluginName.isNotEmpty() ? insert->pluginName
                                                          : juce::File(insert->pluginPath).getFileName(),
                          juce::dontSendNotification);
        bypassButton.setToggleState(insert->bypassed.get(), juce::dontSendNotification);
        bypassButton.setEnabled(true);
        removeButton.setEnabled(true);
        nameLabel.setColour(juce::Label::textColourId, theme::text());
    } else {
        nameLabel.setText("empty \xe2\x80\x94 click to load", juce::dontSendNotification);
        nameLabel.setColour(juce::Label::textColourId, theme::dim());
        bypassButton.setEnabled(false);
        removeButton.setEnabled(false);
    }
    repaint();
}

void ChannelStrip::InsertSlotComponent::resized() {
    auto area = getLocalBounds().reduced(2, 1);
    indexLabel.setBounds(area.removeFromLeft(16));
    removeButton.setBounds(area.removeFromRight(18));
    area.removeFromRight(2);
    bypassButton.setBounds(area.removeFromRight(16));
    area.removeFromRight(2);
    nameLabel.setBounds(area);
}

void ChannelStrip::InsertSlotComponent::mouseDown(const juce::MouseEvent&) {
    if (insert == nullptr) owner.choosePlugin();
}

void ChannelStrip::InsertSlotComponent::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    g.setColour(insert != nullptr ? theme::strip() : theme::control().withAlpha(0.5f));
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
    sourceNameLabel.setText(channel.name, juce::dontSendNotification);
    sourceNameLabel.setJustificationType(juce::Justification::centred);
    sourceNameLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    sourceNameLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(sourceNameLabel);

    deviceCombo.addItem("Default Input", 1);
    deviceCombo.setSelectedId(1, juce::dontSendNotification);
    deviceCombo.setColour(juce::ComboBox::backgroundColourId, theme::control());
    deviceCombo.setColour(juce::ComboBox::textColourId, theme::text());
    deviceCombo.setColour(juce::ComboBox::outlineColourId, theme::border());
    addAndMakeVisible(deviceCombo);

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
    levelLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    levelLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(levelLabel);

    levelKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    levelKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 40, 14);
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
    panLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    panLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(panLabel);

    panKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    panKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 40, 14);
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

    // routing buttons
    const std::vector<juce::String> labels = {"A1","A2","A3","A4","A5","B1","B2","B3"};
    for (const auto& lbl : labels) {
        auto* btn = routingButtons.add(new juce::TextButton(lbl));
        btn->setClickingTogglesState(true);
        btn->setColour(juce::TextButton::buttonColourId, theme::control());
        btn->setColour(juce::TextButton::buttonOnColourId, theme::green());
        btn->setColour(juce::TextButton::textColourOffId, theme::text());
        btn->setColour(juce::TextButton::textColourOnId, juce::Colour(0xff1b2229));
        btn->onClick = [this, btn, lbl] {
            // map label to output index
            int idx = -1;
            if (lbl.startsWith("A")) idx = lbl.substring(1).getIntValue() - 1;
            else if (lbl.startsWith("B")) idx = 5 + lbl.substring(1).getIntValue() - 1;
            const auto& outputs = audioEngine.getOutputs();
            if (idx < 0 || idx >= (int)outputs.size()) return;
            const auto outId = outputs[idx]->id;
            audioEngine.setSendGain(channel.id, outId, btn->getToggleState() ? 0.0f : -60.0f);
        };
        addAndMakeVisible(*btn);
    }

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
    sourceNameLabel.setBounds(area.removeFromTop(22));
    deviceCombo.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);

    vstHeader.setBounds(area.removeFromTop(14));
    // slots
    const int slotH = 18;
    for (int i = 0; i < slots.size(); ++i) {
        slots[i]->setBounds(area.removeFromTop(slotH));
        area.removeFromTop(2);
    }
    addVstButton.setBounds(area.removeFromTop(20));
    area.removeFromTop(6);

    // knobs row
    auto knobRow = area.removeFromTop(56);
    auto leftKnob = knobRow.removeFromLeft(knobRow.getWidth() / 2);
    levelLabel.setBounds(leftKnob.removeFromTop(12));
    levelKnob.setBounds(leftKnob);
    knobRow.removeFromLeft(4);
    panLabel.setBounds(knobRow.removeFromTop(12));
    panKnob.setBounds(knobRow);

    area.removeFromTop(4);
    // routing buttons: 2 rows of 4
    const int btnH = 18;
    for (int i = 0; i < (int)routingButtons.size(); ++i) {
        if (i % 4 == 0) {
            // new row
        }
    }
    // layout in grid
    {
        int idx = 0;
        for (int row = 0; row < 2; ++row) {
            auto r = area.removeFromTop(btnH);
            area.removeFromTop(2);
            for (int col = 0; col < 4; ++col) {
                if (idx < routingButtons.size()) {
                    auto cell = r.removeFromLeft(r.getWidth() / (4 - col));
                    routingButtons[idx]->setBounds(cell.reduced(1));
                    ++idx;
                }
            }
        }
    }
    area.removeFromTop(4);

    // fader + meter
    auto bottom = area.removeFromBottom(30); // mute/solo
    auto fm = area; // remaining for fader+meter
    const int meterW = 12;
    levelMeter.setBounds(fm.removeFromRight(meterW));
    fm.removeFromRight(4);
    faderSlider.setBounds(fm);
    area.removeFromTop(0);

    muteButton.setBounds(bottom.removeFromLeft(bottom.getWidth() / 2));
    bottom.removeFromLeft(4);
    soloButton.setBounds(bottom);
}

void ChannelStrip::refreshInserts() {
    // update slots
    for (int i = 0; i < slots.size(); ++i) {
        const AudioEngine::InsertSlot* ins = nullptr;
        if (i < (int)channel.inserts.size()) ins = channel.inserts[i].get();
        slots[i]->setInsert(ins);
    }
    repaint();
}

void ChannelStrip::refreshRouting() {
    const auto& outputs = audioEngine.getOutputs();
    for (int i = 0; i < (int)routingButtons.size(); ++i) {
        bool on = false;
        if (i < (int)outputs.size()) {
            const float g = audioEngine.getSendGain(channel.id, outputs[i]->id);
            on = g > -30.0f;
            routingButtons[i]->setEnabled(true);
        } else {
            routingButtons[i]->setEnabled(false);
        }
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

void ChannelStrip::paint(juce::Graphics& g) {
    g.fillAll(theme::strip());
    // border
    g.setColour(theme::border());
    g.drawRect(getLocalBounds(), 1);
}
