#include "MixerComponent.h"

MixerComponent::OutputStrip::OutputStrip(AudioEngine::Output& outputState, int outputIdx,
                                                 AudioEngine& engine)
    : output(outputState), outputIndex(outputIdx), audioEngine(engine), meter(outputState.peak) {
    deviceControl.setTitle(output.name);
    deviceControl.setDeviceName(output.outputDeviceName);
    deviceControl.setError(output.outputError);

    removeOutputButton.setFontSize(13.0f);
    removeOutputButton.setClickAction([this] {
        juce::Component::SafePointer<OutputStrip> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe == nullptr) return;
            auto callback = safe->onRemoveRequested;
            if (callback) callback();
        });
    });
    removeOutputButton.setTooltip("Remove this output");
    addAndMakeVisible(removeOutputButton);

    deviceControl.setOnClick([this] {
        DeviceMenu::showOutputMenu(deviceControl, output.outputDeviceName,
                                   audioEngine.getAvailableOutputDevices(),
                                   [this](juce::String chosen) {
            const std::uint32_t outputId = output.id;
            juce::Thread::launch([this, outputId, chosen] {
                const auto err = audioEngine.setOutputDevice(outputId, chosen);
                juce::MessageManager::callAsync([this, err] {
                    deviceControl.setDeviceName(output.outputDeviceName);
                    deviceControl.setError(output.outputError);
                    if (err.isNotEmpty()) {
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
                    }
                });
            });
        });
    });
    deviceControl.setOnRename([this] {
        auto* w = new juce::AlertWindow("Rename output", "Enter new name:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", output.name);
        w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<MixerComponent::OutputStrip> safe(this);
        w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w](int result) {
            if (result == 1 && safe != nullptr) {
                const auto newName = w->getTextEditorContents("name");
                safe->audioEngine.setOutputName(safe->output.id, newName);
                safe->deviceControl.setTitle(newName);
                if (safe->onRenamed) safe->onRenamed();
            }
            delete w;
        }), true);
    });
    addAndMakeVisible(deviceControl);

    modeCombo.addItem("Mono", 1);
    modeCombo.addItem("Stereo", 2);
    modeCombo.setSelectedId(output.layout == pluginrack::ChannelLayout::Mono ? 1 : 2, juce::dontSendNotification);
    modeCombo.setColour(juce::ComboBox::backgroundColourId, theme::control());
    modeCombo.setColour(juce::ComboBox::textColourId, theme::text());
    modeCombo.setColour(juce::ComboBox::arrowColourId, theme::dim());
    modeCombo.setColour(juce::ComboBox::outlineColourId, theme::border());
    modeCombo.onChange = [this] {
        audioEngine.setOutputLayout(output.id,
            modeCombo.getSelectedId() == 1 ? pluginrack::ChannelLayout::Mono
                                          : pluginrack::ChannelLayout::Stereo);
    };
    addAndMakeVisible(modeCombo);

    flipButton.setClickingTogglesState(true);
    flipButton.setToggleState(output.flipped.get(), juce::dontSendNotification);
    flipButton.setColour(juce::TextButton::buttonColourId, theme::control());
    flipButton.setColour(juce::TextButton::buttonOnColourId, theme::cyan());
    flipButton.setColour(juce::TextButton::textColourOffId, theme::text());
    flipButton.setTooltip("Flip L/R of this output");
    flipButton.onClick = [this] {
        audioEngine.setOutputFlipped(output.id, flipButton.getToggleState());
    };
    addAndMakeVisible(flipButton);

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

    gainSlider.setSliderStyle(juce::Slider::LinearVertical);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxAbove, false, 40, 14);
    gainSlider.setRange(-60.0, 12.0, 0.1);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setDoubleClickReturnValue(true, 0.0);
    gainSlider.setValue(output.gainDb.get(), juce::dontSendNotification);
    gainSlider.onValueChange = [this] {
        audioEngine.setOutputGain(output.id, static_cast<float>(gainSlider.getValue()));
    };
    gainSlider.setColour(juce::Slider::trackColourId, theme::deepGreen());
    gainSlider.setColour(juce::Slider::backgroundColourId, theme::strip());
    gainSlider.setColour(juce::Slider::thumbColourId, theme::green());
    gainSlider.setColour(juce::Slider::textBoxTextColourId, theme::text());
    gainSlider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    addAndMakeVisible(gainSlider);

    addAndMakeVisible(meter);

    muteButton.setClickingTogglesState(true);
    muteButton.setToggleState(output.muted.get(), juce::dontSendNotification);
    muteButton.setColour(juce::TextButton::buttonColourId, theme::control());
    muteButton.setColour(juce::TextButton::buttonOnColourId, theme::red());
    muteButton.setColour(juce::TextButton::textColourOffId, theme::text());
    muteButton.onClick = [this] {
        audioEngine.setOutputMuted(output.id, muteButton.getToggleState());
    };
    addAndMakeVisible(muteButton);

    eqButton.setColour(juce::TextButton::buttonColourId, theme::control());
    eqButton.setColour(juce::TextButton::textColourOffId, theme::text());
    eqButton.setTooltip("Open the output EQ editor");
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
}

MixerComponent::OutputStrip::~OutputStrip() {
    closeEqWindow();
}

void MixerComponent::OutputStrip::openEqWindow() {
    if (eqWindow != nullptr) {
        eqWindow->toFront(true);
        return;
    }
    const std::uint32_t outputId = output.id;
    juce::Component::SafePointer<OutputStrip> safe(this);
    // Live device rate for the curve's magnitude maths (see ChannelStrip).
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
    // setOutputEq is keyed by scene-order position (Task 3 asymmetry), so
    // resolve the position by id at edit time rather than trusting the
    // cached strip index across open->edit gaps. Engagement follows edits,
    // same rationale as the channel strip.
    auto* window = new EQWindow(
        "EQ - " + output.name, output.eqBands,
        [safe, outputId](const std::vector<pluginrack::EqBand>& bands) {
            if (safe == nullptr) return;
            const auto& outs = safe->audioEngine.getOutputs();
            for (int i = 0; i < static_cast<int>(outs.size()); ++i) {
                if (outs[static_cast<std::size_t>(i)]->id == outputId) {
                    safe->audioEngine.setOutputEqBypassed(i, false);
                    safe->audioEngine.setOutputEq(i, bands);
                    break;
                }
            }
            safe->refreshEqButton();
        },
        liveRateHz);
    eqWindow = window;
    window->onCloseRequest = [safe] {
        if (safe == nullptr) return;
        safe->closeEqWindow();
    };
    refreshEqButton();
}

void MixerComponent::OutputStrip::closeEqWindow() {
    if (eqWindow == nullptr) return;
    auto* doomed = eqWindow;
    eqWindow = nullptr;
    delete doomed;
    refreshEqButton();
}

void MixerComponent::OutputStrip::refreshEqButton() {
    bool active = false;
    if (!output.eqBypassed.get()) {
        for (const auto& band : output.eqBands) {
            if (band.enabled) {
                active = true;
                break;
            }
        }
    }
    const bool open = (eqWindow != nullptr);
    eqButton.setColour(juce::TextButton::buttonColourId,
                       active ? theme::amber() : theme::control());
    eqButton.setColour(juce::TextButton::textColourOffId,
                       active ? theme::bg() : theme::text());
    eqButton.setRingVisible(open);
}

void MixerComponent::OutputStrip::resized() {
    auto area = getLocalBounds().reduced(4);
    auto headerRow = area.removeFromTop(22);
    auto btnArea = headerRow.removeFromRight(22);
    removeOutputButton.setBounds(btnArea);
    area.removeFromTop(4);
    deviceControl.setBounds(area.removeFromTop(46));
    area.removeFromTop(4);

    auto modeRow = area.removeFromTop(20);
    modeCombo.setBounds(modeRow.removeFromLeft(modeRow.getWidth() / 2 - 2));
    modeRow.removeFromLeft(4);
    flipButton.setBounds(modeRow);
    area.removeFromTop(4);

    vstHeader.setBounds(area.removeFromTop(14));
    const int slotH = 22;
    for (int i = 0; i < (int)slots.size(); ++i) {
        slots[i]->setBounds(area.removeFromTop(slotH));
        area.removeFromTop(2);
    }
    addVstButton.setBounds(area.removeFromTop(20));
    area.removeFromTop(6);

    auto mb = area.removeFromBottom(22);
    const int half = (mb.getWidth() - 4) / 2;
    eqButton.setBounds(mb.removeFromLeft(half));
    mb.removeFromLeft(4);
    muteButton.setBounds(mb);
    area.removeFromBottom(4);
    // meter right
    auto meterArea = area.removeFromRight(12);
    area.removeFromRight(4);
    meter.setBounds(meterArea);
    gainSlider.setBounds(area);
}

void MixerComponent::OutputStrip::refresh() {
    gainSlider.setValue(output.gainDb.get(), juce::dontSendNotification);
    muteButton.setToggleState(output.muted.get(), juce::dontSendNotification);
    deviceControl.setDeviceName(output.outputDeviceName);
    deviceControl.setError(output.outputError);
    modeCombo.setSelectedId(output.layout == pluginrack::ChannelLayout::Mono ? 1 : 2, juce::dontSendNotification);
    flipButton.setToggleState(output.flipped.get(), juce::dontSendNotification);
    refreshEqButton();
}

void MixerComponent::OutputStrip::refreshInserts() {
    for (int i = 0; i < (int)slots.size(); ++i) {
        AudioEngine::InsertSlot* ins = nullptr;
        if (i < (int)output.inserts.size()) ins = output.inserts[i].get();
        slots[i]->setInsert(ins);
    }
    repaint();
}

juce::String MixerComponent::OutputStrip::insertPlugin(const juce::File& file) {
    const auto error = audioEngine.addOutputPlugin(output.id, file);
    refreshAfterInsertChange();
    return error;
}

void MixerComponent::OutputStrip::erasePlugin(std::uint32_t insertId) {
    audioEngine.removeOutputPlugin(output.id, insertId);
}

void MixerComponent::OutputStrip::setPluginBypass(std::uint32_t insertId, bool bypassed) {
    audioEngine.setOutputPluginBypassed(output.id, insertId, bypassed);
}

void MixerComponent::OutputStrip::refreshAfterInsertChange() {
    refreshInserts();
    juce::Component::SafePointer<MixerComponent::OutputStrip> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        auto callback = safe->onRenamed;
        // reuse onRenamed as the structure-changed callback for outputs
        if (callback) callback();
    });
}

void MixerComponent::OutputStrip::openPicker() {
    fileChooser = std::make_unique<juce::FileChooser>(
        "Select a VST3 plugin", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.vst3");
    juce::Component::SafePointer<OutputStrip> safeThis(this);
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

MixerComponent::MixerComponent(AudioEngine& engine)
    : audioEngine(engine) {
    addChannelButton.onClick = [this] {
        audioEngine.addChannel();
        rebuild();
        if (onStructureChanged) onStructureChanged();
    };
    addChannelButton.setColour(juce::TextButton::buttonColourId, theme::control());
    addChannelButton.setColour(juce::TextButton::textColourOffId, theme::text());
    addAndMakeVisible(addChannelButton);

    addOutputButton.onClick = [this] {
        audioEngine.addOutput();
        rebuild();
        if (onStructureChanged) onStructureChanged();
    };
    addOutputButton.setColour(juce::TextButton::buttonColourId, theme::control());
    addOutputButton.setColour(juce::TextButton::textColourOffId, theme::text());
    addAndMakeVisible(addOutputButton);

    channelViewport.setViewedComponent(&stripContent, false);
    channelViewport.setScrollBarsShown(false, true);
    addAndMakeVisible(channelViewport);

    masterHeader.setJustificationType(juce::Justification::centred);
    masterHeader.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    masterHeader.setColour(juce::Label::textColourId, theme::amber());
    masterPanel.addAndMakeVisible(masterHeader);

    masterViewport.setViewedComponent(&masterContent, false);
    masterViewport.setScrollBarsShown(false, true);
    masterPanel.addAndMakeVisible(masterViewport);
    addAndMakeVisible(masterPanel);

    rebuild();
}

void MixerComponent::resized() {
    auto area = getLocalBounds().reduced(4);
    auto toolbar = area.removeFromTop(28);
    addChannelButton.setBounds(toolbar.removeFromLeft(60));
    toolbar.removeFromLeft(4);
    addOutputButton.setBounds(toolbar.removeFromLeft(60));
    area.removeFromTop(4);

    const int masterW = juce::jlimit(0, area.getWidth(), 3 * (120 + 4) + 24);
    masterPanel.setBounds(area.removeFromRight(juce::jmax(0, masterW)));
    area.removeFromRight(6);
    channelViewport.setBounds(area);

    // strip content size
    const int stripW = 148 + 6;
    const int contentW = juce::jmax(area.getWidth(), (int)strips.size() * stripW + 8);
    int stripH = area.getHeight();
    if (contentW > channelViewport.getWidth()) {
        stripH = juce::jmax(0, stripH - channelViewport.getScrollBarThickness());
    }
    stripContent.setSize(juce::jmax(0, contentW), juce::jmax(0, stripH));
    for (int i = 0; i < (int)strips.size(); ++i) {
        strips[i]->setBounds(i * stripW + 4, 0, 148, stripH);
    }

    // master content
    auto ma = masterPanel.getLocalBounds();
    masterHeader.setBounds(ma.removeFromTop(24));
    masterViewport.setBounds(ma);
    const int outW = 120;
    const int outGap = 4;
    const int contentW2 = (int)outputStrips.size() * (outW + outGap) + 8;
    int masterH = masterViewport.getHeight();
    if (contentW2 > masterViewport.getWidth()) {
        masterH = juce::jmax(0, masterH - masterViewport.getScrollBarThickness());
    }
    masterContent.setSize(juce::jmax(0, juce::jmax(masterViewport.getWidth(), contentW2)),
                         juce::jmax(0, masterH));
    for (int i = 0; i < (int)outputStrips.size(); ++i) {
        outputStrips[i]->setBounds(i * (outW + outGap) + 4, 0, outW, masterH);
    }
}

void MixerComponent::rebuild() {
    strips.clear();
    for (const auto& ch : audioEngine.getChannels()) {
        auto* s = strips.add(new ChannelStrip(*ch, audioEngine));
        const std::uint32_t id = ch->id;
        s->onRemoveRequested = [this, id] {
            audioEngine.removeChannel(id);
            rebuild();
            if (onStructureChanged) onStructureChanged();
        };
        s->rebuildCallback = [this] {
            // Strips call this after engine-side structure edits (e.g. split).
            rebuild();
            if (onStructureChanged) onStructureChanged();
        };
        stripContent.addAndMakeVisible(*s);
    }

    outputStrips.clear();
    int outputIndex = 0;
    for (const auto& out : audioEngine.getOutputs()) {
        auto* s = outputStrips.add(new OutputStrip(*out, outputIndex++, audioEngine));
        const std::uint32_t id = out->id;
        s->onRemoveRequested = [this, id] {
            const auto err = audioEngine.removeOutput(id);
            if (err.isNotEmpty()) {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
                return;
            }
            rebuild();
            if (onStructureChanged) onStructureChanged();
        };
        s->onRenamed = [this] {
            rebuild();
            if (onStructureChanged) onStructureChanged();
        };
        masterContent.addAndMakeVisible(*s);
    }
    resized();
}
