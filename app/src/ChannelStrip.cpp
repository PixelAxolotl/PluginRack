#include "ChannelStrip.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kStripWidth = 148;
}

ChannelStrip::ChannelStrip(AudioEngine::Channel& channelState, AudioEngine& engine)
    : channel(channelState),
      audioEngine(engine),
      levelMeter(channelState.peak) {
    deviceControl.setTitle(channel.name);
    deviceControl.setDeviceName(channel.inputDeviceName);
    deviceControl.setError(channel.inputError);
    refreshInput();

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

    splitButton.setFontSize(8.0f);
    splitButton.setClickAction([this] { splitIntoMonoChannels(); });
    splitButton.setTooltip("Split into mono channels - one strip per physical input");
    addAndMakeVisible(splitButton);

    deviceControl.setOnClick([this] {
        DeviceMenu::showInputMenu(deviceControl, channel.inputDeviceName,
                                  audioEngine.getAvailableInputDevices(),
                                  [this](juce::String chosen) {
            const std::uint32_t channelId = channel.id;
            juce::Thread::launch([this, channelId, chosen] {
                const auto err = audioEngine.setChannelInputDevice(channelId, chosen);
                juce::MessageManager::callAsync([this, err] {
                    refreshInput();
                    if (err.isNotEmpty()) {
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
                    }
                });
            });
        });
    });
    deviceControl.setOnRename([this] {
        auto* w = new juce::AlertWindow("Rename channel", "Enter new name:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", channel.name);
        w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<ChannelStrip> safe(this);
        w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w](int result) {
            if (result == 1 && safe != nullptr) {
                const auto newName = w->getTextEditorContents("name");
                safe->audioEngine.setChannelName(safe->channel.id, newName);
                safe->deviceControl.setTitle(newName);
                if (safe->rebuildCallback) safe->rebuildCallback();
            }
            delete w;
        }), true);
    });
    addAndMakeVisible(deviceControl);

    modeCombo.addItem("Mono", 1);
    modeCombo.addItem("Stereo", 2);
    for (int i = 1; i <= 16; ++i) {
        inputComboA.addItem(juce::String(i), i);
        inputComboB.addItem(juce::String(i), i);
    }
    inputComboA.setTooltip("Physical input for this channel (1-based)");
    inputComboB.setTooltip("Physical input for the right side (1-based)");
    const auto styleCombo = [this](juce::ComboBox& combo) {
        combo.setColour(juce::ComboBox::backgroundColourId, theme::control());
        combo.setColour(juce::ComboBox::textColourId, theme::text());
        combo.setColour(juce::ComboBox::arrowColourId, theme::dim());
        combo.setColour(juce::ComboBox::outlineColourId, theme::border());
        combo.onChange = [this] { applyInputMapping(); };
        addAndMakeVisible(combo);
    };
    styleCombo(modeCombo);
    styleCombo(inputComboA);
    styleCombo(inputComboB);

    inputErrorLabel.setFont(juce::Font(juce::FontOptions(9.0f)));
    inputErrorLabel.setJustificationType(juce::Justification::centredLeft);
    inputErrorLabel.setColour(juce::Label::textColourId, theme::red());
    inputErrorLabel.setVisible(false);
    addAndMakeVisible(inputErrorLabel);

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
    addVstButton.onClick = [this] { openPicker(); };
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

    eqButton.setColour(juce::TextButton::buttonColourId, theme::control());
    eqButton.setColour(juce::TextButton::textColourOffId, theme::text());
    eqButton.setTooltip("Open the channel EQ editor");
    eqButton.onClick = [this] {
        if (eqWindow == nullptr) {
            openEqWindow();
        } else {
            closeEqWindow();
        }
    };
    addAndMakeVisible(eqButton);
    refreshEqButton();

    refreshInserts();
    refreshRouting();
    refreshInput();
}

ChannelStrip::~ChannelStrip() {
    closeEqWindow();
}

void ChannelStrip::openEqWindow() {
    if (eqWindow != nullptr) {
        eqWindow->toFront(true);
        return;
    }
    auto* window = new EQWindow("EQ - " + channel.name);
    eqWindow = window;
    juce::Component::SafePointer<ChannelStrip> safe(this);
    window->onCloseRequest = [safe] {
        if (safe == nullptr) return;
        safe->closeEqWindow();
    };
    refreshEqButton();
}

void ChannelStrip::closeEqWindow() {
    if (eqWindow == nullptr) return;
    auto* doomed = eqWindow;
    eqWindow = nullptr;
    delete doomed;
    refreshEqButton();
}

void ChannelStrip::refreshEqButton() {
    bool active = false;
    if (!channel.eqBypassed.get()) {
        for (const auto& band : channel.eqBands) {
            if (band.enabled) {
                active = true;
                break;
            }
        }
    }
    const bool open = (eqWindow != nullptr);
    eqButton.setColour(juce::TextButton::buttonColourId,
                       open ? theme::cyan() : (active ? theme::amber() : theme::control()));
    eqButton.setColour(juce::TextButton::textColourOffId,
                       (open || active) ? theme::bg() : theme::text());
}

void ChannelStrip::resized() {
    auto area = getLocalBounds().reduced(4);

    // header row
    auto headerRow = area.removeFromTop(22);
    auto btnArea = headerRow.removeFromRight(22);
    auto splitArea = headerRow.removeFromRight(48);
    channelLabel.setBounds(headerRow);
    removeChannelButton.setBounds(btnArea);
    splitButton.setBounds(splitArea);
    area.removeFromTop(4);

    deviceControl.setBounds(area.removeFromTop(40));
    area.removeFromTop(4);

    // input mapping row: [Mono/Stereo] .... [In A][In B]
    auto inputRow = area.removeFromTop(20);
    inputRow.removeFromTop(1);
    modeCombo.setBounds(inputRow.removeFromLeft(58));
    if (inputComboB.isVisible()) {
        inputComboB.setBounds(inputRow.removeFromRight(34));
        inputRow.removeFromRight(4);
    }
    inputComboA.setBounds(inputRow.removeFromRight(34));
    if (inputErrorLabel.isVisible()) {
        inputErrorLabel.setBounds(area.removeFromTop(14));
    }
    area.removeFromTop(2);

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
    auto bottom = area.removeFromBottom(30); // eq/mute/solo thirds
    const int third = (bottom.getWidth() - 8) / 3;
    auto fm = area; // remaining for fader+meter
    if (fm.getHeight() < 140) {
        // keep minimum; area already consumed? leave as is
    }
    const int meterW = 12;
    levelMeter.setBounds(fm.removeFromRight(meterW));
    fm.removeFromRight(4);
    faderSlider.setBounds(fm);

    muteButton.setBounds(bottom.removeFromLeft(third));
    bottom.removeFromLeft(4);
    soloButton.setBounds(bottom.removeFromLeft(third));
    bottom.removeFromLeft(4);
    eqButton.setBounds(bottom);
}

void ChannelStrip::refreshInserts() {
    // update slots
    for (int i = 0; i < slots.size(); ++i) {
        AudioEngine::InsertSlot* ins = nullptr;
        if (i < (int)channel.inserts.size()) ins = channel.inserts[i].get();
        slots[i]->setInsert(ins);
    }
    const auto hottest = audioEngine.getHighestLatencyInsert(channel.id);
    if (hottest.latencySamples > 0) {
        const double sampleRate = audioEngine.getLatencyReport().sampleRate;
        const double ms = hottest.latencySamples / (sampleRate > 0.0 ? sampleRate : 48000.0) * 1000.0;
        vstHeader.setText("VST RACK - hottest " + hottest.name + " "
                              + juce::String((int)std::round(ms)) + " ms",
                          juce::dontSendNotification);
    } else {
        vstHeader.setText("VST RACK", juce::dontSendNotification);
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

void ChannelStrip::refreshInput() {
    const bool mono = channel.layout == pluginrack::ChannelLayout::Mono;
    modeCombo.setSelectedId(mono ? 1 : 2, juce::dontSendNotification);
    const int a = channel.inputChannels.empty() ? 1 : channel.inputChannels[0] + 1;
    inputComboA.setSelectedId(juce::jlimit(1, 16, a), juce::dontSendNotification);
    const int b = channel.inputChannels.size() > 1 ? channel.inputChannels[1] + 1 : a;
    inputComboB.setSelectedId(juce::jlimit(1, 16, b), juce::dontSendNotification);
    inputComboB.setVisible(!mono);
    deviceControl.setDeviceName(channel.inputDeviceName);
    deviceControl.setError(channel.inputError);
    inputErrorLabel.setText(channel.inputError, juce::dontSendNotification);
    inputErrorLabel.setVisible(channel.inputError.isNotEmpty());
    resized();
}

void ChannelStrip::applyInputMapping() {
    const bool mono = modeCombo.getSelectedId() == 1;
    const int a = juce::jmax(0, juce::jmax(1, inputComboA.getSelectedId()) - 1);
    std::vector<int> mapping;
    if (mono) {
        mapping = {a};
    } else {
        int b = juce::jmax(0, juce::jmax(1, inputComboB.getSelectedId()) - 1);
        if (b == a) b = (a == 0 ? 1 : 0);
        mapping = {a, b};
    }
    audioEngine.setChannelMapping(channel.id,
                                 mono ? pluginrack::ChannelLayout::Mono : pluginrack::ChannelLayout::Stereo,
                                 mapping);
    refreshInput();
}

void ChannelStrip::splitIntoMonoChannels() {
    const auto device = channel.inputDeviceName;
    // The capture only stores what was requested; the device may expose more
    // physical inputs than this channel's mapping uses.
    const int inputs = channel.input != nullptr ? channel.input->availableInputCount() : 0;
    if (device.isEmpty() || inputs < 2) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon, "PluginRack",
            "Split needs an input device with at least two available physical inputs.");
        return;
    }

    juce::Component::SafePointer<ChannelStrip> safe(this);
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("PluginRack")
            .withMessage("Split into mono channels? The " + juce::String(device) + " device has "
                          + juce::String(inputs) + " inputs; each gets its own mono strip.")
            .withButton("Split")
            .withButton("Cancel"),
        [safe, device, inputs](int result) {
            if (result == 0 || safe == nullptr) return;
            auto& engine = safe->audioEngine;
            const std::uint32_t firstId = safe->channel.id;
            engine.setChannelMapping(firstId, pluginrack::ChannelLayout::Mono, {0});
            engine.setChannelName(firstId, "Input 1");
            for (int i = 1; i < inputs; ++i) {
                engine.addChannel();
                const auto& chans = engine.getChannels();
                if (chans.empty()) break;
                const std::uint32_t newId = chans.back()->id;
                engine.setChannelName(newId, "Input " + juce::String(i + 1));
                engine.setChannelMapping(newId, pluginrack::ChannelLayout::Mono, {i});
                const auto err = engine.setChannelInputDevice(newId, device);
                if (err.isNotEmpty()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
                }
            }
            // Copy the callback: this strip is destroyed when the mixer rebuilds.
            auto callback = safe->rebuildCallback;
            juce::MessageManager::callAsync([callback] { if (callback) callback(); });
        });
}

void ChannelStrip::openPicker() {
    fileChooser = std::make_unique<juce::FileChooser>(
        "Select a VST3 plugin", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.vst3");
    juce::Component::SafePointer<ChannelStrip> safeThis(this);
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

juce::String ChannelStrip::insertPlugin(const juce::File& file) {
    const auto error = audioEngine.addPlugin(channel.id, file);
    refreshAfterInsertChange();
    return error;
}

void ChannelStrip::erasePlugin(std::uint32_t insertId) {
    audioEngine.removePlugin(channel.id, insertId);
}

void ChannelStrip::setPluginBypass(std::uint32_t insertId, bool bypassed) {
    audioEngine.setPluginBypassed(channel.id, insertId, bypassed);
}

void ChannelStrip::refreshAfterInsertChange() {
    refreshInserts();
    juce::Component::SafePointer<ChannelStrip> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        auto callback = safe->rebuildCallback;
        if (callback) callback();
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
