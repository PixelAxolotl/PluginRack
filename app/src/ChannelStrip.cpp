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
    channelLabel.setInterceptsMouseClicks(false, false);
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
            deviceControl.setOnDoubleClick([this] {
                if (onDoubleClicked) onDoubleClicked();
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

    modeCombo.addItem("MONO", 1);
    modeCombo.addItem("ST", 2);
    for (int i = 1; i <= 16; ++i) {
        inputComboA.addItem("A" + juce::String(i), i);
        inputComboB.addItem("B" + juce::String(i), i);
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

    levelLabel.setText("TRIM", juce::dontSendNotification);
    levelLabel.setJustificationType(juce::Justification::centred);
    levelLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    levelLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(levelLabel);

    levelKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    levelKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
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
    levelKnob.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    addAndMakeVisible(levelKnob);

    panLabel.setText("PAN", juce::dontSendNotification);
    panLabel.setJustificationType(juce::Justification::centred);
    panLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    panLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(panLabel);

    panKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    panKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
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
    panKnob.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    addAndMakeVisible(panKnob);

    rebuildRoutingButtons();

    faderSlider.setSliderStyle(juce::Slider::LinearVertical);
    faderSlider.setTextBoxStyle(juce::Slider::TextBoxAbove, false, 64, 20);
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
    faderSlider.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
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
    eqButton.setTooltip("Click: EQ on/off. Right-click: open editor");
    eqButton.onClick = [this] {
        // Left-click toggles the EQ engine on/off (Voicemeeter-style).
        const bool bypassed = channel.eqBypassed.get();
        audioEngine.setChannelEqBypassed(channel.id, !bypassed);
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
    refreshRouting();
    refreshInput();
}

ChannelStrip::~ChannelStrip() {
    closeEqWindow();
    if (dynamicsWindow != nullptr) {
        delete dynamicsWindow;
        dynamicsWindow = nullptr;
    }
}

void ChannelStrip::openDynamicsWindow(pluginrack::DynamicsEffectType type) {
    if (dynamicsWindow != nullptr) {
        dynamicsWindow->toFront(true);
        return;
    }
    juce::Component::SafePointer<ChannelStrip> safe(this);
    auto it = std::find_if(channel.effects.begin(), channel.effects.end(),
                           [type](const auto& effect) { return effect.type == type; });
    if (it == channel.effects.end()) return;
    const auto gateIt = std::find_if(channel.effects.begin(), channel.effects.end(),
                                     [](const auto& effect) {
                                         return effect.type == pluginrack::DynamicsEffectType::Gate;
                                     });
    dynamicsWindow = new DynamicsWindow(
        "Dynamics - " + juce::String(it->type == pluginrack::DynamicsEffectType::Compressor
                                         ? "Compressor"
                                         : "Gate"),
        *it,
        [safe](const pluginrack::DynamicsEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto effects = safe->channel.effects;
            const auto found = std::find_if(effects.begin(), effects.end(),
                                            [&effect](const auto& candidate) {
                                                return candidate.id == effect.id;
                                            });
            if (found != effects.end()) {
                const bool bypassed = found->bypassed;
                *found = effect;
                if (!bypassChanged) found->bypassed = bypassed;
                safe->audioEngine.setChannelEffects(safe->channel.id, effects);
                safe->refreshDynamicsButtons();
            }
        },
        gateIt != channel.effects.end() ? &*gateIt : nullptr);
    dynamicsWindow->onClose = [safe] {
        if (safe != nullptr && safe->dynamicsWindow != nullptr) {
            safe->channel.analyzerTap.setMode(AudioEngine::AnalyzerTap::kOff);
            auto* window = safe->dynamicsWindow;
            safe->dynamicsWindow = nullptr;
            delete window;
        }
    };
    dynamicsWindow->pullAnalyzerTap = [safe](float* left, float* right, int frames) {
        if (safe != nullptr) {
            safe->channel.analyzerTap.setMode(AudioEngine::AnalyzerTap::kPre);
            safe->channel.analyzerTap.snapshot(false, left, right, frames);
        }
    };
    channel.analyzerTap.setMode(AudioEngine::AnalyzerTap::kPre);
    dynamicsWindow->setVisible(true);
}

void ChannelStrip::openEqWindow() {
    if (eqWindow != nullptr) {
        eqWindow->toFront(true);
        return;
    }
    juce::Component::SafePointer<ChannelStrip> safe(this);
    // Live device rate so the curve's magnitude maths matches what the engine
    // applies; getLatencyReport().sampleRate is the UI-thread convention
    // already used for the latency readout.
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
    // Bands are snapshotted at open time (same semantics as the title);
    // every in-window edit lands here. Editing implies engagement: the
    // engine starts bypassed (scene default eqEnabled=false) and the
    // window is the app's only engage path, so a drag must be audible.
    auto* window = new EQWindow(
        "EQ - " + channel.name, channel.eqBands,
        [safe](const std::vector<pluginrack::EqBand>& bands) {
            if (safe == nullptr) return;
            safe->audioEngine.setChannelEqBypassed(safe->channel.id, false);
            safe->audioEngine.setChannelEq(safe->channel.id, bands);
            safe->refreshEqButton();
        },
        liveRateHz);
    // Analyzer wiring (Task 6): the window pulls this channel's tap ring copy
    // on its UI timer and gates which side render() memcpy-copies; the
    // kAnalyzer* mode codes mirror AudioEngine::AnalyzerTap::kOff/kPre/kPost.
    window->pullAnalyzerTap = [safe](bool postEq, float* destL, float* destR) {
        if (safe == nullptr) return;
        safe->channel.analyzerTap.snapshot(postEq, destL, destR,
                                           EQWindow::kAnalyzerFftSize);
    };
    window->setAnalyzerTapMode = [safe](int mode) {
        if (safe == nullptr) return;
        safe->channel.analyzerTap.setMode(static_cast<std::uint32_t>(mode));
    };
    // Start the analyzer AFTER the tap callbacks above are wired: the mode
    // call forwards into the ring, and before this point it addressed no one
    // (this ordering was the "dead RTA until mode toggle" bug). Fixed Post.
    window->setAnalyzerMode(EQWindow::kAnalyzerPost);
    // Delay knob: seeded from the strip, persisted through the engine.
    // Editing engages the EQ like a band drag does, so the new delay is
    // audible in the window the user is looking at.
    window->setDelayMs(channel.eqDelayMs);
    window->applyDelayMs = [safe](float delayMs) {
        if (safe == nullptr) return;
        safe->audioEngine.setChannelEqBypassed(safe->channel.id, false);
        safe->audioEngine.setChannelEqDelay(safe->channel.id, delayMs);
        safe->refreshEqButton();
    };
    eqWindow = window;
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
        // A delay-only EQ still does audible work, so it lights the button.
        active = channel.eqDelayMs > 0.0f;
        for (const auto& band : channel.eqBands) {
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

void ChannelStrip::toggleDynamicsBypass(pluginrack::DynamicsEffectType type) {
    auto effects = channel.effects;
    const auto found = std::find_if(effects.begin(), effects.end(),
                                    [type](const auto& effect) { return effect.type == type; });
    if (found == effects.end()) return;
    found->bypassed = !found->bypassed;
    audioEngine.setChannelEffects(channel.id, effects);
    refreshDynamicsButtons();
}

void ChannelStrip::refreshDynamicsButtons() {
    const auto apply = [this](EqButton& button, pluginrack::DynamicsEffectType type) {
        const auto found = std::find_if(channel.effects.begin(), channel.effects.end(),
                                        [type](const auto& effect) { return effect.type == type; });
        const bool active = found != channel.effects.end() && !found->bypassed;
        if (found != channel.effects.end() && dynamicsWindow != nullptr &&
            dynamicsWindow->getType() == type) {
            dynamicsWindow->setBypassed(found->bypassed);
            if (type == pluginrack::DynamicsEffectType::Compressor) {
                const auto gateIt = std::find_if(
                    channel.effects.begin(), channel.effects.end(), [](const auto& effect) {
                        return effect.type == pluginrack::DynamicsEffectType::Gate;
                    });
                if (gateIt != channel.effects.end()) dynamicsWindow->setGate(*gateIt);
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

void ChannelStrip::resized() {
    auto area = getLocalBounds().reduced(4);

    // header row: CH label, SPLIT, X
    auto headerRow = area.removeFromTop(22);
    auto btnArea = headerRow.removeFromRight(22);
    auto splitArea = headerRow.removeFromRight(48);
    channelLabel.setBounds(headerRow);
    removeChannelButton.setBounds(btnArea);
    splitButton.setBounds(splitArea);
    area.removeFromTop(4);

    // Input source row (Input v)
    deviceControl.setBounds(area.removeFromTop(32));
    area.removeFromTop(4);

    // input channel mapping row
    auto inputRow = area.removeFromTop(20);
    inputRow.removeFromTop(1);
    modeCombo.setBounds(inputRow.removeFromLeft(50));
    if (inputComboB.isVisible()) {
        inputComboB.setBounds(inputRow.removeFromRight(40));
        inputRow.removeFromRight(4);
    }
    inputComboA.setBounds(inputRow.removeFromRight(40));
    if (inputErrorLabel.isVisible()) {
        inputErrorLabel.setBounds(area.removeFromTop(14));
    }
    area.removeFromTop(2);

    // Gain row: trim knob with the input mapping kept above it.
    auto gainRow = area.removeFromTop(74);
    auto gainLeft = gainRow.removeFromLeft((gainRow.getWidth() * 2) / 5);
    gainRow.removeFromLeft(4);
    levelLabel.setBounds(gainLeft.removeFromTop(14));
    levelKnob.setBounds(gainLeft.reduced(2));
    area.removeFromTop(4);

    // Pan row: pan knob (left) + EQ button (right)
    auto panRow = area.removeFromTop(74);
    auto panLeft = panRow.removeFromLeft((panRow.getWidth() * 2) / 5);
    panRow.removeFromLeft(4);
    panLabel.setBounds(panLeft.removeFromTop(14));
    panKnob.setBounds(panLeft.reduced(2));
    area.removeFromTop(4);

    auto effectStack = area.removeFromTop(3 * 20 + 2 * 3);
    auto placeEffectButton = [&effectStack](juce::Component& button) {
        button.setBounds(effectStack.removeFromTop(20));
        effectStack.removeFromTop(3);
    };
    placeEffectButton(gateButton);
    placeEffectButton(eqButton);
    placeEffectButton(compressorButton);
    area.removeFromTop(4);

    // Plugin / insert slot container
    vstHeader.setBounds(area.removeFromTop(14));
    const int slotH = 20;
    for (int i = 0; i < (int)slots.size(); ++i) {
        slots[i]->setBounds(area.removeFromTop(slotH));
        area.removeFromTop(2);
    }
    addVstButton.setBounds(area.removeFromTop(18));
    area.removeFromTop(4);

    // routing buttons (compact)
    const int rows = juce::jmax(1, ((int)routingButtons.size() + 3) / 4);
    const int routingHeight = rows * 20;
    auto routingArea = area.removeFromTop(routingHeight);
    const int cellW = juce::jmax(1, routingArea.getWidth() / 4);
    for (int i = 0; i < (int)routingButtons.size(); ++i) {
        const int row = i / 4;
        const int col = i % 4;
        routingButtons[i]->setBounds(routingArea.getX() + col * cellW + 1,
                                    routingArea.getY() + row * 20,
                                    juce::jmax(1, cellW - 2), 18);
    }
    area.removeFromTop(4);

    // fader + meter (bottom, dominant)
    auto footer = area.removeFromBottom(30); // M / S
    const int half = (footer.getWidth() - 4) / 2;
    auto fm = area;
    const int meterW = 12;
    levelMeter.setBounds(fm.removeFromRight(meterW));
    fm.removeFromRight(4);
    faderSlider.setBounds(fm);

    muteButton.setBounds(footer.removeFromLeft(half));
    footer.removeFromLeft(4);
    soloButton.setBounds(footer);
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
    const bool hasInputDevice = channel.inputDeviceName.isNotEmpty();
    modeCombo.setSelectedId(mono ? 1 : 2, juce::dontSendNotification);
    const int a = channel.inputChannels.empty() ? 1 : channel.inputChannels[0] + 1;
    inputComboA.setSelectedId(juce::jlimit(1, 16, a), juce::dontSendNotification);
    const int b = channel.inputChannels.size() > 1 ? channel.inputChannels[1] + 1 : a;
    inputComboB.setSelectedId(juce::jlimit(1, 16, b), juce::dontSendNotification);
    modeCombo.setVisible(hasInputDevice);
    inputComboA.setVisible(hasInputDevice);
    inputComboB.setVisible(hasInputDevice && !mono);
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
    // Mirror OutputStrip: the rack may have docked an editor owned by this
    // insert. Release-only on purpose (see InsertHost.h): a rebuild here
    // would re-dock the doomed insert. Null-safe; a second release from the
    // slot's remove button is a no-op.
    if (auto release = pluginRemovingHook()) release();
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
    auto content = getLocalBounds().reduced(1);
    g.setColour(theme::panel().withAlpha(0.42f));
    g.fillRect(content.removeFromTop(24));
    // border
    g.setColour(theme::border());
    g.drawRect(getLocalBounds(), 1);
}

void ChannelStrip::mouseDown(const juce::MouseEvent&) {
    if (onSelected) onSelected();
}

void ChannelStrip::mouseDoubleClick(const juce::MouseEvent&) {
    if (onDoubleClicked) onDoubleClicked();
}
