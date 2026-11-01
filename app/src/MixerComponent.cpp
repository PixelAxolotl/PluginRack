#include "MixerComponent.h"

#include <algorithm>

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

    flipButton.setToggleState(output.flipped.get());
    flipButton.setTooltip("Flip L/R of this output");
    flipButton.setClickAction([this] {
        const bool next = !flipButton.getToggleState();
        flipButton.setToggleState(next);
        audioEngine.setOutputFlipped(output.id, next);
    });
    addAndMakeVisible(flipButton);

    vstHeader.setJustificationType(juce::Justification::centredLeft);
    vstHeader.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    vstHeader.setColour(juce::Label::textColourId, theme::violet());
    addAndMakeVisible(vstHeader);

    gainSlider.setSliderStyle(juce::Slider::LinearVertical);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxAbove, false, 64, 20);
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
    gainSlider.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
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
    eqButton.setTooltip("Click: EQ on/off. Right-click: open editor");
    eqButton.onClick = [this] {
        // Left-click toggles the EQ engine on/off (Voicemeeter-style).
        const bool bypassed = output.eqBypassed.get();
        audioEngine.setOutputEqBypassed(outputIndex, !bypassed);
        refreshEqButton();
    };
    eqButton.onRightClick = [this] {
        // Right-click opens the editor (fronts it if already open).
        openEqWindow();
    };
    addAndMakeVisible(eqButton);
    refreshEqButton();
    const auto setupDynamicsButton = [this](EqButton& button,
                                             pluginrack::DynamicsEffectType type,
                                             const juce::String& tooltip) {
        button.setColour(juce::TextButton::buttonColourId, theme::control());
        button.setColour(juce::TextButton::textColourOffId, theme::text());
        button.setTooltip(tooltip);
        button.onClick = [this, type] { toggleDynamicsBypass(type); };
        button.onRightClick = [this, type] { openDynamicsWindow(type); };
        addAndMakeVisible(button);
    };
    setupDynamicsButton(compressorButton, pluginrack::DynamicsEffectType::Compressor,
                        "Click: compressor on/off. Right-click: open settings");
    setupDynamicsButton(gateButton, pluginrack::DynamicsEffectType::Gate,
                        "Click: gate on/off. Right-click: open the compressor panel's gate row");
    // The gate lives in the compressor panel now, so both right-clicks land there.
    gateButton.onRightClick = [this] { openDynamicsWindow(pluginrack::DynamicsEffectType::Compressor); };
    refreshDynamicsButtons();

    refreshInserts();
}

MixerComponent::OutputStrip::~OutputStrip() {
    closeEqWindow();
    if (dynamicsWindow != nullptr) {
        delete dynamicsWindow;
        dynamicsWindow = nullptr;
    }
}

void MixerComponent::OutputStrip::openDynamicsWindow(pluginrack::DynamicsEffectType type) {
    if (dynamicsWindow != nullptr) {
        dynamicsWindow->toFront(true);
        return;
    }
    juce::Component::SafePointer<OutputStrip> safe(this);
    const auto outputId = output.id;
    auto it = std::find_if(output.effects.begin(), output.effects.end(),
                           [type](const auto& effect) { return effect.type == type; });
    if (it == output.effects.end()) return;
    const auto gateIt = std::find_if(output.effects.begin(), output.effects.end(),
                                     [](const auto& effect) {
                                         return effect.type == pluginrack::DynamicsEffectType::Gate;
                                     });
    dynamicsWindow = new DynamicsWindow(
        "Dynamics - " + output.name, *it,
        [safe, outputId](const pluginrack::DynamicsEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto effects = safe->output.effects;
            const auto found = std::find_if(effects.begin(), effects.end(),
                                            [&effect](const auto& candidate) {
                                                return candidate.id == effect.id;
                                            });
            if (found != effects.end()) {
                const bool bypassed = found->bypassed;
                *found = effect;
                if (!bypassChanged) found->bypassed = bypassed;
                safe->audioEngine.setOutputEffects(outputId, effects);
                safe->refreshDynamicsButtons();
            }
        },
        gateIt != output.effects.end() ? &*gateIt : nullptr);
    dynamicsWindow->onClose = [safe] {
        if (safe != nullptr && safe->dynamicsWindow != nullptr) {
            safe->output.analyzerTap.setMode(AudioEngine::AnalyzerTap::kOff);
            auto* window = safe->dynamicsWindow;
            safe->dynamicsWindow = nullptr;
            delete window;
        }
    };
    dynamicsWindow->pullAnalyzerTap = [safe](float* left, float* right, int frames) {
        if (safe != nullptr) {
            safe->output.analyzerTap.setMode(AudioEngine::AnalyzerTap::kPre);
            safe->output.analyzerTap.snapshot(false, left, right, frames);
        }
    };
    output.analyzerTap.setMode(AudioEngine::AnalyzerTap::kPre);
    dynamicsWindow->setVisible(true);
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
    // Analyzer wiring (Task 6), same contract as ChannelStrip's hunks: pull
    // copies this output's tap ring, setMode gates the render-side write.
    window->pullAnalyzerTap = [safe](bool postEq, float* destL, float* destR) {
        if (safe == nullptr) return;
        safe->output.analyzerTap.snapshot(postEq, destL, destR,
                                          EQWindow::kAnalyzerFftSize);
    };
    window->setAnalyzerTapMode = [safe](int mode) {
        if (safe == nullptr) return;
        safe->output.analyzerTap.setMode(static_cast<std::uint32_t>(mode));
    };
    // Same ordering fix as ChannelStrip: start Post-EQ analysis after the
    // tap callbacks are wired, otherwise the mode call addresses no one.
    window->setAnalyzerMode(EQWindow::kAnalyzerPost);
    // Delay knob, same contract as the channel strip's: seed from the strip
    // and persist through the engine, engaging the EQ on edit.
    window->setDelayMs(output.eqDelayMs);
    window->applyDelayMs = [safe, outputId](float delayMs) {
        if (safe == nullptr) return;
        const auto& outs = safe->audioEngine.getOutputs();
        for (int i = 0; i < static_cast<int>(outs.size()); ++i) {
            if (outs[static_cast<std::size_t>(i)]->id == outputId) {
                safe->audioEngine.setOutputEqBypassed(i, false);
                safe->audioEngine.setOutputEqDelay(i, delayMs);
                break;
            }
        }
        safe->refreshEqButton();
    };
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
        // A delay-only EQ still does audible work, so it lights the button.
        active = output.eqDelayMs > 0.0f;
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

void MixerComponent::OutputStrip::toggleDynamicsBypass(pluginrack::DynamicsEffectType type) {
    auto effects = output.effects;
    const auto found = std::find_if(effects.begin(), effects.end(),
                                    [type](const auto& effect) { return effect.type == type; });
    if (found == effects.end()) return;
    found->bypassed = !found->bypassed;
    audioEngine.setOutputEffects(output.id, effects);
    refreshDynamicsButtons();
}

void MixerComponent::OutputStrip::refreshDynamicsButtons() {
    const auto apply = [this](EqButton& button, pluginrack::DynamicsEffectType type) {
        const auto found = std::find_if(output.effects.begin(), output.effects.end(),
                                        [type](const auto& effect) { return effect.type == type; });
        const bool active = found != output.effects.end() && !found->bypassed;
        if (found != output.effects.end() && dynamicsWindow != nullptr &&
            dynamicsWindow->getType() == type) {
            dynamicsWindow->setBypassed(found->bypassed);
            if (type == pluginrack::DynamicsEffectType::Compressor) {
                const auto gateIt = std::find_if(
                    output.effects.begin(), output.effects.end(), [](const auto& effect) {
                        return effect.type == pluginrack::DynamicsEffectType::Gate;
                    });
                if (gateIt != output.effects.end()) dynamicsWindow->setGate(*gateIt);
            }
        }
        button.setColour(juce::TextButton::buttonColourId,
                         active ? theme::amber() : theme::control());
        button.setColour(juce::TextButton::textColourOffId,
                         active ? theme::bg() : theme::text());
    };
    apply(compressorButton, pluginrack::DynamicsEffectType::Compressor);
    apply(gateButton, pluginrack::DynamicsEffectType::Gate);
}

void MixerComponent::OutputStrip::resized() {
    auto area = getLocalBounds().reduced(4);

    // Same rows and heights as the input strip: cross and flip on the short
    // action row, name + device under it, then the mode field.
    auto actionsRow = area.removeFromTop(theme::metrics::kActionH);
    removeOutputButton.setBounds(actionsRow.removeFromRight(theme::metrics::kCloseW));
    actionsRow.removeFromRight(2);
    flipButton.setBounds(actionsRow.removeFromRight(theme::metrics::kActionW));
    area.removeFromTop(2);
    deviceControl.setBounds(area.removeFromTop(theme::metrics::kDeviceH));
    area.removeFromTop(4);

    modeCombo.setBounds(area.removeFromTop(theme::metrics::kFieldH));
    area.removeFromTop(4);

    // display block: unified 8-slot chain (3 built-in + 5 VST slots).
    // Rows give height back before they squeeze the fader off the strip.
    vstHeader.setBounds(area.removeFromTop(14));
    const int reservedBelow = 22 + 4 + 110;
    const int rowCount = juce::jmax(1, slots.size());
    const int chainSpace = juce::jmax(0, area.getHeight() - 8 - reservedBelow);
    const int slotH = juce::jlimit(14, 20, (chainSpace - 2 * rowCount) / rowCount);
    for (int i = 0; i < (int)slots.size(); ++i) {
        slots[i]->setBounds(area.removeFromTop(slotH));
        area.removeFromTop(2);
    }
    area.removeFromTop(4);
    gateButton.setVisible(false);
    eqButton.setVisible(false);
    compressorButton.setVisible(false);
    area.removeFromTop(4);

    // mute at very bottom, matching the input strip's mute/solo height
    auto muteRow = area.removeFromBottom(theme::metrics::kRowH);
    muteButton.setBounds(muteRow);

    // fader + meter (shorter, ~half of input height) above mute
    area.removeFromBottom(4);
    auto meterArea = area.removeFromRight(12);
    area.removeFromRight(4);
    meter.setBounds(meterArea);
    gainSlider.setBounds(area);
}

void MixerComponent::OutputStrip::paint(juce::Graphics& g) {
    g.fillAll(theme::strip());
    g.setColour(theme::border());
    g.drawRect(getLocalBounds(), 1);
    if (chainDropTarget >= 0) {
        g.setColour(theme::cyan());
        g.fillRoundedRectangle(4.0f, static_cast<float>(chainBoundaryY(chainDropTarget)),
                               static_cast<float>(juce::jmax(0, getWidth() - 8)), 2.0f, 1.0f);
    }
}

void MixerComponent::OutputStrip::refresh() {
    gainSlider.setValue(output.gainDb.get(), juce::dontSendNotification);
    muteButton.setToggleState(output.muted.get(), juce::dontSendNotification);
    deviceControl.setDeviceName(output.outputDeviceName);
    deviceControl.setError(output.outputError);
    modeCombo.setSelectedId(output.layout == pluginrack::ChannelLayout::Mono ? 1 : 2, juce::dontSendNotification);
    flipButton.setToggleState(output.flipped.get());
    refreshEqButton();
}

void MixerComponent::OutputStrip::refreshInserts() {
    chainRowCount = static_cast<int>(output.chain.size());
    const int totalRows = juce::jmax(kChainSlotCount, chainRowCount);
    while (slots.size() < totalRows) {
        auto* slot = slots.add(new InsertSlotComponent(*this, slots.size()));
        addAndMakeVisible(*slot);
    }
    while (slots.size() > totalRows) {
        removeChildComponent(slots.getLast());
        slots.removeLast();
    }
    for (int i = chainRowCount; i < slots.size(); ++i)
        slots[i]->setAddSlot(i == chainRowCount);
    for (int i = 0; i < chainRowCount; ++i) {
        const auto item = output.chain[static_cast<std::size_t>(i)];
        if (item.type == pluginrack::ChainItemType::Insert) {
            auto it = std::find_if(output.inserts.begin(), output.inserts.end(),
                                   [id = item.id](const auto& insert) { return insert->id == id; });
            slots[i]->setInsert(it == output.inserts.end() ? nullptr : it->get());
        } else if (item.type == pluginrack::ChainItemType::Eq) {
            slots[i]->setBuiltIn(item, "EQ", output.eqBypassed.get());
        } else {
            const auto it = std::find_if(output.effects.begin(), output.effects.end(),
                                         [id = item.id](const auto& effect) { return effect.id == id; });
            slots[i]->setBuiltIn(item, item.id == 1 ? "Compressor" : "Gate",
                                 it != output.effects.end() && it->bypassed);
        }
    }
    repaint();
}

void MixerComponent::OutputStrip::mouseDown(const juce::MouseEvent&) {
    if (onSelected) onSelected();
}

void MixerComponent::OutputStrip::mouseDoubleClick(const juce::MouseEvent&) {
    if (onDoubleClicked) onDoubleClicked();
}

juce::String MixerComponent::OutputStrip::insertPlugin(const juce::File& file) {
    const auto error = audioEngine.addOutputPlugin(output.id, file);
    refreshAfterInsertChange();
    return error;
}

void MixerComponent::OutputStrip::erasePlugin(std::uint32_t insertId) {
    // The rack may have docked an editor owned by this insert: release it
    // while the processor is still alive. Release-only on purpose (see
    // InsertHost.h): a rebuild here would re-dock the doomed insert.
    if (auto release = pluginRemovingHook()) release();
    audioEngine.removeOutputPlugin(output.id, insertId);
}

void MixerComponent::OutputStrip::setPluginBypass(std::uint32_t insertId, bool bypassed) {
    audioEngine.setOutputPluginBypassed(output.id, insertId, bypassed);
}

void MixerComponent::OutputStrip::reorderInsert(std::uint32_t insertId, int targetIndex) {
    audioEngine.moveOutputChainItem(output.id, {pluginrack::ChainItemType::Insert, insertId},
                                    targetIndex);
    refreshAfterInsertChange();
}

void MixerComponent::OutputStrip::reorderChainItem(pluginrack::ChainItem item, int targetIndex) {
    audioEngine.moveOutputChainItem(output.id, item, targetIndex);
    juce::Component::SafePointer<OutputStrip> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        // Copy the callback: the rebuild destroys this strip.
        auto callback = safe->onRenamed;
        safe->chainDropTarget = -1;
        safe->refreshInserts();
        if (callback) callback();
    });
}

int MixerComponent::OutputStrip::chainBoundaryY(int targetIndex) const {
    if (chainRowCount <= 0) return vstHeader.getBottom() + 1;
    const int target = juce::jlimit(0, chainRowCount, targetIndex);
    if (target < chainRowCount) return juce::jmax(0, slots[target]->getY() - 2);
    return slots[chainRowCount - 1]->getBottom();
}

int MixerComponent::OutputStrip::chainDropIndexAt(juce::Point<int> parentPoint) {
    int index = 0;
    for (int i = 0; i < chainRowCount; ++i)
        if (parentPoint.y > slots[i]->getBounds().getCentreY()) index = i + 1;
    return index;
}

void MixerComponent::OutputStrip::previewChainItem(int targetIndex) {
    chainDropTarget = juce::jlimit(0, chainRowCount, targetIndex);
    repaint();
}

void MixerComponent::OutputStrip::clearChainPreview() {
    chainDropTarget = -1;
    repaint();
}

void MixerComponent::OutputStrip::setBuiltInBypass(pluginrack::ChainItem item, bool bypassed) {
    if (item.type == pluginrack::ChainItemType::Eq) {
        audioEngine.setOutputEqBypassed(outputIndex, bypassed);
        return;
    }
    auto effects = output.effects;
    for (auto& effect : effects)
        if (effect.id == item.id) effect.bypassed = bypassed;
    audioEngine.setOutputEffects(output.id, effects);
    // Keep an open dynamics panel's gate row in step with the chain row.
    refreshDynamicsButtons();
}

void MixerComponent::OutputStrip::activateBuiltIn(pluginrack::ChainItem item) {
    if (item.type == pluginrack::ChainItemType::Eq)
        openEqWindow();
    // Gate and Compressor share one panel; it is where the gate row lives.
    else
        openDynamicsWindow(pluginrack::DynamicsEffectType::Compressor);
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
    theme::ui::styleButton(addOutputButton);
    addAndMakeVisible(addOutputButton);

    channelViewport.setViewedComponent(&stripContent, false);
    channelViewport.setScrollBarsShown(false, true);
    addAndMakeVisible(channelViewport);

    masterHeader.setText("OUTPUTS", juce::dontSendNotification);
    masterHeader.setJustificationType(juce::Justification::centredLeft);
    masterHeader.setFont(theme::ui::sectionFont());
    masterHeader.setColour(juce::Label::textColourId, theme::dim().withAlpha(0.8f));
    masterPanel.addAndMakeVisible(masterHeader);

    inputHeader.setJustificationType(juce::Justification::centredLeft);
    inputHeader.setFont(theme::ui::sectionFont());
    inputHeader.setColour(juce::Label::textColourId, theme::dim().withAlpha(0.8f));
    addAndMakeVisible(inputHeader);

    masterViewport.setViewedComponent(&masterContent, false);
    masterViewport.setScrollBarsShown(false, true);
    masterPanel.addAndMakeVisible(masterViewport);
    addAndMakeVisible(masterPanel);

    rebuild();
}

void MixerComponent::resized() {
    const bool showInputs = sections_ != Sections::OutputsOnly;
    const bool showOutputs = sections_ != Sections::InputsOnly;

    addChannelButton.setVisible(showInputs);
    addOutputButton.setVisible(showOutputs);
    inputHeader.setVisible(showInputs);
    masterHeader.setVisible(showOutputs);
    channelViewport.setVisible(showInputs);
    masterPanel.setVisible(showOutputs);

    auto area = getLocalBounds().reduced(4);
    auto toolbar = area.removeFromTop(28);
    if (showInputs) {
        addChannelButton.setBounds(toolbar.removeFromLeft(60));
        toolbar.removeFromLeft(4);
    }
    if (showOutputs) addOutputButton.setBounds(toolbar.removeFromLeft(60));
    area.removeFromTop(4);

    if (showInputs && showOutputs) {
        const int masterW = juce::jlimit(0, area.getWidth(), 3 * (120 + 4) + 24);
        masterPanel.setBounds(area.removeFromRight(juce::jmax(0, masterW)));
        area.removeFromRight(6);
        inputHeader.setBounds(area.removeFromTop(theme::ui::kSectionH));
        channelViewport.setBounds(area);
        layoutChannelStrips(area);
        layoutOutputStrips(masterPanel.getLocalBounds());
    } else if (showInputs) {
        inputHeader.setBounds(area.removeFromTop(theme::ui::kSectionH));
        channelViewport.setBounds(area);
        layoutChannelStrips(area);
    } else {
        masterPanel.setBounds(area);
        layoutOutputStrips(masterPanel.getLocalBounds());
    }
}

void MixerComponent::layoutChannelStrips(juce::Rectangle<int> viewportBounds) {
    const int stripW = 148 + 6;
    const int contentW = juce::jmax(viewportBounds.getWidth(), (int)strips.size() * stripW + 8);
    int stripH = viewportBounds.getHeight();
    if (contentW > channelViewport.getWidth()) {
        stripH = juce::jmax(0, stripH - channelViewport.getScrollBarThickness());
    }
    stripContent.setSize(juce::jmax(0, contentW), juce::jmax(0, stripH));
    for (int i = 0; i < (int)strips.size(); ++i) {
        strips[i]->setBounds(i * stripW + 4, 0, 148, stripH);
    }
}

void MixerComponent::layoutOutputStrips(juce::Rectangle<int> panelBounds) {
    masterHeader.setBounds(panelBounds.removeFromTop(theme::ui::kSectionH));
    masterViewport.setBounds(panelBounds);
    const int outW = 120;
    const int outGap = 4;
    const int contentW = (int)outputStrips.size() * (outW + outGap) + 8;
    int masterH = masterViewport.getHeight();
    if (contentW > masterViewport.getWidth()) {
        masterH = juce::jmax(0, masterH - masterViewport.getScrollBarThickness());
    }
    masterContent.setSize(juce::jmax(0, juce::jmax(masterViewport.getWidth(), contentW)),
                          juce::jmax(0, masterH));
    for (int i = 0; i < (int)outputStrips.size(); ++i) {
        outputStrips[i]->setBounds(i * (outW + outGap) + 4, 0, outW, masterH);
    }
}

void MixerComponent::closeAllEqWindows() {
    for (auto* s : strips) {
        if (s != nullptr) s->closeEqWindow();
    }
    for (auto* s : outputStrips) {
        if (s != nullptr) s->closeEqWindow();
    }
}

void MixerComponent::setSections(Sections sections) {
    if (sections_ == sections) return;
    sections_ = sections;
    rebuild();
}

void MixerComponent::rebuild() {
    strips.clear();
    if (sections_ != Sections::OutputsOnly) rebuildChannelStrips();

    outputStrips.clear();
    if (sections_ != Sections::InputsOnly) rebuildOutputStrips();

    resized();
}

void MixerComponent::rebuildChannelStrips() {
    for (const auto& ch : audioEngine.getChannels()) {
        auto* s = strips.add(new ChannelStrip(*ch, audioEngine));
        const std::uint32_t id = ch->id;
        s->onRemoveRequested = [this, id] {
            // The rack may have docked an editor owned by this strip: release it
            // while the processors are still alive. Release-only on purpose (see
            // InsertHost.h): a rebuild here would re-dock a doomed insert.
            if (auto release = pluginRemovingHook()) release();
            audioEngine.removeChannel(id);
            rebuild();
            if (onStructureChanged) onStructureChanged();
        };
        s->rebuildCallback = [this] {
            // Strips call this after engine-side structure edits (e.g. split).
            rebuild();
            if (onStructureChanged) onStructureChanged();
        };
        s->onSelected = [this, id] {
            if (onSelectionChanged) onSelectionChanged(static_cast<int>(id), false);
        };
        s->onDoubleClicked = [this, id] {
            if (onDoubleClicked) onDoubleClicked(static_cast<int>(id), false);
        };
        stripContent.addAndMakeVisible(*s);
    }
}

void MixerComponent::rebuildOutputStrips() {
    int outputIndex = 0;
    for (const auto& out : audioEngine.getOutputs()) {
        auto* s = outputStrips.add(new OutputStrip(*out, outputIndex++, audioEngine));
        const std::uint32_t id = out->id;
        s->onRemoveRequested = [this, id] {
            // The rack may have docked an editor owned by this strip: release it
            // while the processors are still alive. Release-only on purpose (see
            // InsertHost.h): a rebuild here would re-dock a doomed insert.
            if (auto release = pluginRemovingHook()) release();
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
        s->onSelected = [this, id] {
            if (onSelectionChanged) onSelectionChanged(static_cast<int>(id), true);
        };
        s->onDoubleClicked = [this, id] {
            if (onDoubleClicked) onDoubleClicked(static_cast<int>(id), true);
        };
        masterContent.addAndMakeVisible(*s);
    }
}
