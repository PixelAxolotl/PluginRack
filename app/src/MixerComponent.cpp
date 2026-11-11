#include "MixerComponent.h"

#include <algorithm>

#include "DeEsserWindow.h"
#include "LimiterWindow.h"
#include "MultibandWindow.h"

MixerComponent::OutputStrip::OutputStrip(AudioEngine::Output& outputState, int outputIdx,
                                                 AudioEngine& engine)
    : output(outputState), outputIndex(outputIdx), audioEngine(engine),
      meter(gainSlider, outputState.peakL, outputState.peakR, outputState.rmsL,
            outputState.rmsR) {
    deviceControl.setTitle(output.name);
    {
        const auto deviceState = audioEngine.getOutputDeviceState(output.id);
        deviceControl.setDeviceName(deviceState.deviceName);
        deviceControl.setBackendType(deviceState.deviceType);
        deviceControl.setError(deviceState.error);
    }
    meter.setStereo(output.layout != pluginrack::ChannelLayout::Mono);

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
        DeviceMenu::showOutputMenu(deviceControl,
                                   audioEngine.getOutputDeviceState(output.id).deviceName,
                                   audioEngine.getAvailableOutputDevices(),
                                   [this](juce::String chosen) {
            const std::uint32_t outputId = output.id;
            juce::Thread::launch([this, outputId, chosen] {
                const auto err = audioEngine.setOutputDevice(outputId, chosen);
                juce::MessageManager::callAsync([this, err] {
                    const auto deviceState = audioEngine.getOutputDeviceState(output.id);
                    deviceControl.setDeviceName(deviceState.deviceName);
                    deviceControl.setBackendType(deviceState.deviceType);
                    deviceControl.setError(deviceState.error);
                    if (err.isNotEmpty()) {
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
                    }

                });
            });
        });
    });
    deviceControl.setOnRename([this] {
        juce::Component::SafePointer<MixerComponent::OutputStrip> safe(this);
        StripDialog::show("Rename output", output.name, output.colourHue,
                          [safe](const juce::String& newName, float newHue) {
            if (safe == nullptr) return;
            safe->audioEngine.setOutputName(safe->output.id, newName);
            safe->audioEngine.setOutputColourHue(safe->outputIndex, newHue);
            safe->deviceControl.setTitle(newName);
            safe->repaint();
            if (safe->onRenamed) safe->onRenamed();
        });
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
        const bool mono = modeCombo.getSelectedId() == 1;
        audioEngine.setOutputLayout(output.id,
            mono ? pluginrack::ChannelLayout::Mono : pluginrack::ChannelLayout::Stereo);
        meter.setStereo(!mono);
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
    const auto outputAccent = theme::stripColour(output.colourHue);
    gainSlider.setColour(juce::Slider::trackColourId, outputAccent);
    gainSlider.setColour(juce::Slider::backgroundColourId, theme::strip());
    gainSlider.setColour(juce::Slider::thumbColourId, outputAccent);
    gainSlider.setColour(juce::Slider::textBoxTextColourId, theme::text());
    gainSlider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    gainSlider.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    addAndMakeVisible(gainSlider);
    gainSlider.setName("Output");

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

    // All built-in state lives in the insert cards now; no standalone
    // EQ/Comp/Gate buttons remain on the strip.

    refreshInserts();
}

MixerComponent::OutputStrip::~OutputStrip() {
    closeAllEffectWindows();
}

void MixerComponent::OutputStrip::closeAllEffectWindows() {
    closeEqWindow();
    if (dynamicsWindow != nullptr) {
        auto* doomed = dynamicsWindow;
        dynamicsWindow = nullptr;
        delete doomed;
    }
    if (multibandWindow != nullptr) {
        auto* doomed = multibandWindow;
        multibandWindow = nullptr;
        delete doomed;
    }
    if (deEsserWindow != nullptr) {
        auto* doomed = deEsserWindow;
        deEsserWindow = nullptr;
        delete doomed;
    }
    if (limiterWindow != nullptr) {
        auto* doomed = limiterWindow;
        limiterWindow = nullptr;
        delete doomed;
    }
}

void MixerComponent::OutputStrip::openMultibandWindow() {
    if (multibandWindow != nullptr) {
        multibandWindow->toFront(true);
        return;
    }
    if (output.multibandEffects.empty()) {
        auto effects = output.multibandEffects;
        effects.push_back(pluginrack::makeDefaultMultibandEffect(1));
        audioEngine.setOutputMultiband(output.id, effects);
        audioEngine.ensureOutputChainItem(output.id, {pluginrack::ChainItemType::Multiband, 1});
    }
    juce::Component::SafePointer<OutputStrip> safe(this);
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
    multibandWindow = new MultibandWindow(
        "Multiband Compressor - " + output.name, output.multibandEffects.front(),
        [safe](const pluginrack::MultibandEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto effects = safe->output.multibandEffects;
            for (auto& e : effects)
                if (e.id == effect.id) {
                    const bool bp = e.bypassed;
                    e = effect;
                    if (!bypassChanged) e.bypassed = bp;
                }
            safe->audioEngine.setOutputMultiband(safe->output.id, effects);
            safe->refreshInserts();
        },
        output.multibandRtaOffsetDb, output.multibandRtaWeighting, liveRateHz);
    multibandWindow->applySpectrumOffset = [safe](float offsetDb) {
        if (safe != nullptr) safe->audioEngine.setOutputMultibandRtaOffset(safe->output.id, offsetDb);
    };
    multibandWindow->applyWeighting = [safe](int weighting) {
        if (safe != nullptr) safe->audioEngine.setOutputMultibandRtaWeighting(safe->output.id, weighting);
    };
    multibandWindow->onClose = [safe] {
        if (safe != nullptr && safe->multibandWindow != nullptr) {
            auto* w = safe->multibandWindow;
            safe->multibandWindow = nullptr;
            delete w;
        }
    };
    multibandWindow->pullAnalyzerTap = [safe](float* l, float* r, int n) {
        if (safe != nullptr) safe->output.multibandTap.snapshot(true, l, r, n);
    };
    output.multibandTap.setMode(AudioEngine::AnalyzerTap::kPost);
}

void MixerComponent::OutputStrip::openDeEsserWindow() {
    if (deEsserWindow != nullptr) {
        deEsserWindow->toFront(true);
        return;
    }
    if (output.deEsserEffects.empty()) {
        auto effects = output.deEsserEffects;
        effects.push_back(pluginrack::makeDefaultDeEsserEffect(1));
        audioEngine.setOutputDeEssers(output.id, effects);
        audioEngine.ensureOutputChainItem(output.id, {pluginrack::ChainItemType::DeEsser, 1});
    }
    juce::Component::SafePointer<OutputStrip> safe(this);
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
    deEsserWindow = new DeEsserWindow(
        "De-esser - " + output.name, output.deEsserEffects.front(),
        [safe](const pluginrack::DeEsserEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto effects = safe->output.deEsserEffects;
            for (auto& e : effects)
                if (e.id == effect.id) {
                    const bool bp = e.bypassed;
                    e = effect;
                    if (!bypassChanged) e.bypassed = bp;
                }
            safe->audioEngine.setOutputDeEssers(safe->output.id, effects);
            safe->refreshInserts();
        },
        output.deEsserRtaOffsetDb, output.deEsserRtaWeighting, liveRateHz);
    deEsserWindow->applySpectrumOffset = [safe](float offsetDb) {
        if (safe != nullptr) safe->audioEngine.setOutputDeEsserRtaOffset(safe->output.id, offsetDb);
    };
    deEsserWindow->applyWeighting = [safe](int weighting) {
        if (safe != nullptr) safe->audioEngine.setOutputDeEsserRtaWeighting(safe->output.id, weighting);
    };
    deEsserWindow->onClose = [safe] {
        if (safe != nullptr && safe->deEsserWindow != nullptr) {
            auto* w = safe->deEsserWindow;
            safe->deEsserWindow = nullptr;
            delete w;
        }
    };
    deEsserWindow->pullAnalyzerTap = [safe](float* l, float* r, int n) {
        if (safe != nullptr) safe->output.deEsserTap.snapshot(true, l, r, n);
    };
    output.deEsserTap.setMode(AudioEngine::AnalyzerTap::kPost);
}

void MixerComponent::OutputStrip::openLimiterWindow() {
    if (limiterWindow != nullptr) {
        limiterWindow->toFront(true);
        return;
    }
    if (output.limiterEffects.empty()) {
        auto effects = output.limiterEffects;
        effects.push_back(pluginrack::makeDefaultLimiterEffect(1));
        audioEngine.setOutputLimiters(output.id, effects);
        audioEngine.ensureOutputChainItem(output.id, {pluginrack::ChainItemType::Limiter, 1});
    }
    juce::Component::SafePointer<OutputStrip> safe(this);
    limiterWindow = new LimiterWindow(
        "Limiter - " + output.name, output.limiterEffects.front(),
        [safe](const pluginrack::LimiterEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto effects = safe->output.limiterEffects;
            for (auto& e : effects)
                if (e.id == effect.id) {
                    const bool bp = e.bypassed;
                    e = effect;
                    if (!bypassChanged) e.bypassed = bp;
                }
            safe->audioEngine.setOutputLimiters(safe->output.id, effects);
            safe->refreshInserts();
        });
    limiterWindow->onClose = [safe] {
        if (safe != nullptr && safe->limiterWindow != nullptr) {
            auto* w = safe->limiterWindow;
            safe->limiterWindow = nullptr;
            delete w;
        }
    };
    limiterWindow->pullAnalyzerTap = [safe](bool /*postEq*/, float* l, float* r, int n) {
        if (safe != nullptr) safe->output.limiterTap.snapshot(true, l, r, n);
    };
    output.limiterTap.setMode(AudioEngine::AnalyzerTap::kPost);
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
            safe->output.dynamicsTap.setMode(AudioEngine::AnalyzerTap::kOff);
            auto* window = safe->dynamicsWindow;
            safe->dynamicsWindow = nullptr;
            delete window;
        }
    };
    dynamicsWindow->pullAnalyzerTap = [safe](float* left, float* right, int frames) {
        if (safe != nullptr) {
            safe->output.dynamicsTap.setMode(AudioEngine::AnalyzerTap::kPost);
            safe->output.dynamicsTap.snapshot(true, left, right, frames);
        }
    };
    output.dynamicsTap.setMode(AudioEngine::AnalyzerTap::kPost);
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
        liveRateHz, false, output.eqRtaOffsetDb, output.eqRtaWeighting);
    window->applySpectrumOffset = [safe, outputId](float offsetDb) {
        if (safe == nullptr) return;
        const auto& outs = safe->audioEngine.getOutputs();
        for (int i = 0; i < static_cast<int>(outs.size()); ++i) {
            if (outs[static_cast<std::size_t>(i)]->id == outputId) {
                safe->audioEngine.setOutputEqRtaOffset(i, offsetDb);
                break;
            }
        }
    };
    window->applyWeighting = [safe, outputId](int weighting) {
        if (safe == nullptr) return;
        const auto& outs = safe->audioEngine.getOutputs();
        for (int i = 0; i < static_cast<int>(outs.size()); ++i) {
            if (outs[static_cast<std::size_t>(i)]->id == outputId) {
                safe->audioEngine.setOutputEqRtaWeighting(i, weighting);
                break;
            }
        }
    };
    // Analyzer wiring: the EQ reads its own post-EQ tap (eqTap), so its
    // RTA shows what leaves the EQ — a later plugin can't tint it.
    window->pullAnalyzerTap = [safe](bool postEq, float* destL, float* destR) {
        if (safe == nullptr) return;
        safe->output.eqTap.snapshot(postEq, destL, destR,
                                    EQWindow::kAnalyzerFftSize);
    };
    window->setAnalyzerTapMode = [safe](int mode) {
        if (safe == nullptr) return;
        safe->output.eqTap.setMode(static_cast<std::uint32_t>(mode));
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
    // Phase flip, same contract as the channel strip.
    window->setPhaseFlip(output.eqPhaseFlip.get());
    window->applyPhaseFlip = [safe, outputId](bool flipped) {
        if (safe == nullptr) return;
        const auto& outs = safe->audioEngine.getOutputs();
        for (int i = 0; i < static_cast<int>(outs.size()); ++i) {
            if (outs[static_cast<std::size_t>(i)]->id == outputId) {
                safe->audioEngine.setOutputEqBypassed(i, false);
                safe->audioEngine.setOutputEqPhaseFlip(i, flipped);
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
    // No standalone button left: the insert cards carry the bypass state.
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
    // No standalone buttons left: still syncs an open Dynamics window.
    if (dynamicsWindow == nullptr) return;
    const auto type = dynamicsWindow->getType();
    const auto found = std::find_if(output.effects.begin(), output.effects.end(),
                                    [type](const auto& effect) { return effect.type == type; });
    if (found == output.effects.end()) return;
    dynamicsWindow->setBypassed(found->bypassed);
    if (type == pluginrack::DynamicsEffectType::Compressor) {
        const auto gateIt = std::find_if(
            output.effects.begin(), output.effects.end(), [](const auto& effect) {
                return effect.type == pluginrack::DynamicsEffectType::Gate;
            });
        if (gateIt != output.effects.end()) dynamicsWindow->setGate(*gateIt);
    }
}

void MixerComponent::OutputStrip::resized() {
    auto area = getLocalBounds().reduced(4);

    // Same rows and heights as the input strip: cross and flip on the short
    // action row, name + device under it, then the mode field.
    auto actionsRow = area.removeFromTop(theme::metrics::kActionH);
    // The grip takes its lane before any action does: a button that eats the
    // row would leave it zero-wide and make the strip impossible to drag.
    dragHandleBounds = dragHandleVisible
                           ? actionsRow.removeFromLeft(theme::metrics::kActionH)
                           : juce::Rectangle<int>();
    removeOutputButton.setBounds(actionsRow.removeFromRight(theme::metrics::kCloseW));
    actionsRow.removeFromRight(2);
    flipButton.setBounds(actionsRow.removeFromRight(theme::metrics::kActionW));
    area.removeFromTop(2);
    deviceControl.setBounds(area.removeFromTop(theme::metrics::kDeviceH));
    area.removeFromTop(4);

    modeCombo.setBounds(area.removeFromTop(theme::metrics::kFieldH));
    area.removeFromTop(4);

    // Display block: unified chain (3 built-in + the live inserts).
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
    area.removeFromTop(4);

    // mute at very bottom, matching the input strip's mute/solo height
    auto muteRow = area.removeFromBottom(theme::metrics::kRowH);
    muteButton.setBounds(muteRow);

    // fader + meter (shorter, ~half of input height) above mute. The meter
    // lines its bars up with the fader's travel itself and keeps the readouts
    // in the strip above the travel, so it takes the fader's full height.
    area.removeFromBottom(4);
    auto meterColumn = area.removeFromRight(LevelMeter::kPreferredWidth);
    area.removeFromRight(4);
    gainSlider.setBounds(area);
    meter.setBounds(meterColumn);
}

void MixerComponent::OutputStrip::paint(juce::Graphics& g) {
    theme::drawStripFace(g, getLocalBounds());
    // Identity colour: bottom accent bar in the strip's own hue.
    theme::drawStripAccent(g, getLocalBounds(), output.colourHue);
    paintDragHandle(g);
    if (chainDropTarget >= 0) {
        g.setColour(theme::cyan());
        g.fillRoundedRectangle(4.0f, static_cast<float>(chainBoundaryY(chainDropTarget)),
                               static_cast<float>(juce::jmax(0, getWidth() - 8)), 2.0f, 1.0f);
    }
}

void MixerComponent::OutputStrip::setDragHandleVisible(bool visible) {
    if (dragHandleVisible == visible) return;
    dragHandleVisible = visible;
    draggingHandle = false;
    resized();
    repaint();
}

void MixerComponent::OutputStrip::paintDragHandle(juce::Graphics& g) const {
    if (!dragHandleVisible || dragHandleBounds.isEmpty()) return;
    g.setColour(draggingHandle ? theme::cyan() : theme::dim().withAlpha(0.65f));
    const auto centre = dragHandleBounds.toFloat().getCentre();
    // Two columns of three dots: the same 6-dot grip the input strips carry.
    for (int row = -1; row <= 1; ++row) {
        for (int column = 0; column <= 1; ++column) {
            g.fillEllipse(centre.x - 2.5f + static_cast<float>(column) * 4.0f,
                          centre.y - 3.0f + static_cast<float>(row) * 3.0f, 2.0f, 2.0f);
        }
    }
}

void MixerComponent::OutputStrip::mouseDrag(const juce::MouseEvent& e) {
    if (!draggingHandle) return;
    if (onDragHandleMoved) onDragHandleMoved(e.getScreenPosition());
}

void MixerComponent::OutputStrip::mouseUp(const juce::MouseEvent&) {
    if (!draggingHandle) return;
    draggingHandle = false;
    repaint();
    if (onDragHandleFinished) onDragHandleFinished();
}

void MixerComponent::OutputStrip::mouseMove(const juce::MouseEvent& e) {
    setMouseCursor(dragHandleVisible && dragHandleBounds.contains(e.getPosition())
                       ? juce::MouseCursor::DraggingHandCursor
                       : juce::MouseCursor::NormalCursor);
}

void MixerComponent::OutputStrip::refresh() {
    gainSlider.setValue(output.gainDb.get(), juce::dontSendNotification);
    muteButton.setToggleState(output.muted.get(), juce::dontSendNotification);
    // Locked snapshot: the device opener writes these strings off-thread.
    const auto deviceState = audioEngine.getOutputDeviceState(output.id);
    deviceControl.setDeviceName(deviceState.deviceName);
    deviceControl.setBackendType(deviceState.deviceType);
    deviceControl.setError(deviceState.error);
    const bool mono = output.layout == pluginrack::ChannelLayout::Mono;
    modeCombo.setSelectedId(mono ? 1 : 2, juce::dontSendNotification);
    meter.setStereo(!mono);
    flipButton.setToggleState(output.flipped.get());
    refreshEqButton();
}

void MixerComponent::OutputStrip::refreshInserts() {
    // The compressor and the gate share the Dynamics card, so rows are not a
    // one-to-one map of the chain any more.
    const auto rows = pluginrack::buildChainRows(output.chain, output.effects);
    chainRowCount = static_cast<int>(rows.size());
    const bool canAddVst =
        static_cast<int>(output.inserts.size()) < kChainVstSlots;
    const int totalRows = canAddVst ? chainRowCount + 1 : chainRowCount;
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

    const auto compressor = std::find_if(output.effects.begin(), output.effects.end(),
                                         [](const auto& effect) {
                                             return effect.type
                                                    == pluginrack::DynamicsEffectType::Compressor;
                                         });
    const auto gate = std::find_if(output.effects.begin(), output.effects.end(),
                                   [](const auto& effect) {
                                       return effect.type == pluginrack::DynamicsEffectType::Gate;
                                   });
    for (int r = 0; r < chainRowCount; ++r) {
        const auto& indices = rows[static_cast<std::size_t>(r)];
        const auto item = output.chain[static_cast<std::size_t>(indices.front())];
        if (indices.size() > 1) {
            // Compressor + gate: one switch each, and a click on the row opens
            // the panel that carries both.
            const int compIndex =
                pluginrack::compressorIndexInRow(output.chain, output.effects, indices);
            const int gateIndex = indices.front() == compIndex ? indices.back() : indices.front();
            const auto compItem = output.chain[static_cast<std::size_t>(compIndex)];
            const auto gateItem = output.chain[static_cast<std::size_t>(gateIndex)];
            slots[r]->setBuiltInPair(
                "Dynamics", compItem,
                CardSwitch{ compItem, compressor != output.effects.end() && compressor->bypassed,
                            "C", "Compressor: click to bypass, right-click to open the panel" },
                CardSwitch{ gateItem, gate != output.effects.end() && gate->bypassed, "G",
                            "Gate: click to bypass, right-click to open the panel" });
            continue;
        }
        if (item.type == pluginrack::ChainItemType::Insert) {
            auto it = std::find_if(output.inserts.begin(), output.inserts.end(),
                                   [id = item.id](const auto& insert) { return insert->id == id; });
            slots[r]->setInsert(it == output.inserts.end() ? nullptr : it->get());
        } else if (item.type == pluginrack::ChainItemType::Eq) {
            slots[r]->setBuiltIn(item, "EQ", output.eqBypassed.get());
        } else if (item.type == pluginrack::ChainItemType::Multiband ||
                   item.type == pluginrack::ChainItemType::DeEsser ||
                   item.type == pluginrack::ChainItemType::Limiter) {
            const juce::String name = item.type == pluginrack::ChainItemType::Multiband ? "Multiband Comp" : item.type == pluginrack::ChainItemType::DeEsser ? "De-esser" : "Limiter";
            bool bp = false;
            if (item.type == pluginrack::ChainItemType::Multiband) for (auto& e : output.multibandEffects) if (e.id==item.id) bp=e.bypassed;
            if (item.type == pluginrack::ChainItemType::DeEsser) for (auto& e : output.deEsserEffects) if (e.id==item.id) bp=e.bypassed;
            if (item.type == pluginrack::ChainItemType::Limiter) for (auto& e : output.limiterEffects) if (e.id==item.id) bp=e.bypassed;
            slots[r]->setBuiltIn(item, name, bp);
        } else {
            const auto it = std::find_if(output.effects.begin(), output.effects.end(),
                                         [id = item.id](const auto& effect) { return effect.id == id; });
            const bool isCompressor = it != output.effects.end()
                                          ? it->type == pluginrack::DynamicsEffectType::Compressor
                                          : item.id == 1;
            slots[r]->setBuiltIn(item, isCompressor ? "Compressor" : "Gate",
                                 it != output.effects.end() && it->bypassed);
        }
    }
    repaint();
}

void MixerComponent::OutputStrip::mouseDown(const juce::MouseEvent& e) {
    if (onSelected) onSelected();
    if (dragHandleVisible && e.mods.isLeftButtonDown()
        && dragHandleBounds.contains(e.getPosition())) {
        draggingHandle = true;
        repaint();
        if (onDragHandleStarted) onDragHandleStarted();
    }
}

void MixerComponent::OutputStrip::mouseDoubleClick(const juce::MouseEvent&) {
    if (onDoubleClicked) onDoubleClicked();
}

juce::String MixerComponent::OutputStrip::insertPlugin(const juce::File& file, int pluginUniqueId) {
    // The load is asynchronous; the strip refreshes when it finishes.
    const auto outputId = output.id;
    juce::Component::SafePointer<MixerComponent::OutputStrip> safe(this);
    audioEngine.addOutputPluginAsync(outputId, file, pluginUniqueId, [safe](juce::String error) {
        if (safe == nullptr) return;
        safe->refreshAfterInsertChange();
        if (error.isNotEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "PluginRack", error);
        }
    });
    return {};
}

void MixerComponent::OutputStrip::erasePlugin(std::uint32_t insertId) {
    // The rack bay may describe this insert. Release-only on purpose (see
    // InsertHost.h): a rebuild here would re-describe the doomed insert.
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
    // A card moves as one: collect every chain entry it covers.
    const auto rows = pluginrack::buildChainRows(output.chain, output.effects);
    std::vector<pluginrack::ChainItem> cardItems{ item };
    for (const auto& row : rows) {
        const bool holdsItem = std::any_of(row.begin(), row.end(), [this, &item](int index) {
            return output.chain[static_cast<std::size_t>(index)] == item;
        });
        if (!holdsItem) continue;
        if (row.size() > 1) {
            cardItems.clear();
            for (const int index : row)
                cardItems.push_back(output.chain[static_cast<std::size_t>(index)]);
        }
        break;
    }
    // Plan the card against a simulated chain: the engine erases each item
    // before inserting it, so a drop below the card lands where the marker
    // showed instead of one row past it.
    const auto moves =
        pluginrack::planChainCardMoves(output.chain, rows, targetIndex, cardItems);
    for (const auto& move : moves)
        audioEngine.moveOutputChainItem(output.id, move.item, move.targetIndex);
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
    if (item.type == pluginrack::ChainItemType::Multiband) {
        auto effects = output.multibandEffects;
        for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
        audioEngine.setOutputMultiband(output.id, effects);
        return;
    }
    if (item.type == pluginrack::ChainItemType::DeEsser) {
        auto effects = output.deEsserEffects;
        for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
        audioEngine.setOutputDeEssers(output.id, effects);
        return;
    }
    if (item.type == pluginrack::ChainItemType::Limiter) {
        auto effects = output.limiterEffects;
        for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
        audioEngine.setOutputLimiters(output.id, effects);
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
    else if (item.type == pluginrack::ChainItemType::Multiband)
        openMultibandWindow();
    else if (item.type == pluginrack::ChainItemType::DeEsser)
        openDeEsserWindow();
    else if (item.type == pluginrack::ChainItemType::Limiter)
        openLimiterWindow();
    // Gate and Compressor share one panel; it is where the gate row lives.
    else
        openDynamicsWindow(pluginrack::DynamicsEffectType::Compressor);
}

void MixerComponent::OutputStrip::insertBuiltIn(pluginrack::ChainItem item, int chainIndex) {
    audioEngine.ensureOutputChainItem(output.id, item);
    if (item.type == pluginrack::ChainItemType::Multiband && std::none_of(output.multibandEffects.begin(), output.multibandEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto effects = output.multibandEffects; effects.push_back(pluginrack::makeDefaultMultibandEffect(item.id)); audioEngine.setOutputMultiband(output.id, effects); }
    if (item.type == pluginrack::ChainItemType::DeEsser && std::none_of(output.deEsserEffects.begin(), output.deEsserEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto effects = output.deEsserEffects; effects.push_back(pluginrack::makeDefaultDeEsserEffect(item.id)); audioEngine.setOutputDeEssers(output.id, effects); }
    if (item.type == pluginrack::ChainItemType::Limiter && std::none_of(output.limiterEffects.begin(), output.limiterEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto effects = output.limiterEffects; effects.push_back(pluginrack::makeDefaultLimiterEffect(item.id)); audioEngine.setOutputLimiters(output.id, effects); }
    if (item.type == pluginrack::ChainItemType::Dynamics) {
        for (const auto& effect : output.effects)
            if (effect.id != item.id)
                audioEngine.ensureOutputChainItem(output.id,
                                                  {pluginrack::ChainItemType::Dynamics, effect.id});
    }
    const auto rows = pluginrack::buildChainRows(output.chain, output.effects);
    std::vector<pluginrack::ChainItem> cardItems{ item };
    for (const auto& row : rows) {
        const bool holdsItem = std::any_of(row.begin(), row.end(), [this, &item](int index) {
            return output.chain[static_cast<std::size_t>(index)] == item;
        });
        if (!holdsItem) continue;
        if (row.size() > 1) {
            cardItems.clear();
            for (const int index : row)
                cardItems.push_back(output.chain[static_cast<std::size_t>(index)]);
        }
        break;
    }
    const auto moves =
        pluginrack::planChainCardMoves(output.chain, rows, chainIndex, cardItems);
    for (const auto& move : moves)
        audioEngine.moveOutputChainItem(output.id, move.item, move.targetIndex);
    setBuiltInBypass(item, false);
    refreshAfterInsertChange();
}

void MixerComponent::OutputStrip::eraseBuiltIn(pluginrack::ChainItem item) {
    std::vector<pluginrack::ChainItem> cardItems{ item };
    if (item.type == pluginrack::ChainItemType::Dynamics) {
        const auto rows = pluginrack::buildChainRows(output.chain, output.effects);
        for (const auto& row : rows) {
            const bool holdsItem =
                std::any_of(row.begin(), row.end(), [this, &item](int index) {
                    return output.chain[static_cast<std::size_t>(index)] == item;
                });
            if (!holdsItem || row.size() <= 1) continue;
            cardItems.clear();
            for (const int index : row)
                cardItems.push_back(output.chain[static_cast<std::size_t>(index)]);
            break;
        }
    }
    for (const auto& entry : cardItems)
        audioEngine.removeOutputChainItem(output.id, entry);
    refreshAfterInsertChange();
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
        const int masterW = juce::jlimit(0, area.getWidth(), 3 * (148 + 6) + 24);
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
    // Same width as the input strips: both sides read as one console.
    const int outW = 148;
    const int outGap = 6;
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
        if (s != nullptr) s->closeAllEffectWindows();
    }
    for (auto* s : outputStrips) {
        if (s != nullptr) s->closeAllEffectWindows();
    }
}

void MixerComponent::setSections(Sections sections) {
    if (sections_ == sections) return;
    sections_ = sections;
    rebuild();
}

void MixerComponent::rebuild() {
    channelInsertIndex = -1;
    outputInsertIndex = -1;
    stripContent.dropMarkerX = -1.0f;
    masterContent.dropMarkerX = -1.0f;
    strips.clear();
    if (sections_ != Sections::OutputsOnly) rebuildChannelStrips();

    outputStrips.clear();
    if (sections_ != Sections::InputsOnly) rebuildOutputStrips();

    resized();
}

void MixerComponent::StripLane::paint(juce::Graphics& g) {
    if (dropMarkerX < 0.0f || getHeight() <= 0) return;
    // Same light-blue insertion bar the VST chain drag draws, turned upright for
    // a horizontal row of strips: a bright hairline in the gutter where the
    // strip will land, with a soft halo so it reads against the dark lane.
    const float barHeight = static_cast<float>(juce::jmax(0, getHeight() - 4));
    g.setColour(theme::cyan().withAlpha(0.3f));
    g.fillRoundedRectangle(dropMarkerX - 2.0f, 2.0f, 7.0f, barHeight, 3.5f);
    g.setColour(theme::cyan());
    g.fillRoundedRectangle(dropMarkerX, 2.0f, 3.0f, barHeight, 1.5f);
}

float MixerComponent::insertionBarX(int insertionIndex, int inset, int step, int gap) {
    if (insertionIndex <= 0) return 0.5f; // leading edge, in the lane's own inset
    return static_cast<float>(inset + insertionIndex * step - gap) + (gap - 3.0f) * 0.5f;
}

int MixerComponent::moveTargetIndex(int insertionIndex, int sourceIndex, int count) {
    if (count <= 0) return -1;
    int target = insertionIndex;
    if (target > sourceIndex) --target; // the dragged strip leaves its own slot
    return juce::jlimit(0, count - 1, target);
}

void MixerComponent::updateChannelDrag(juce::Point<int> screenPosition) {
    const int count = strips.size();
    if (count <= 0) return;
    constexpr int stripWidth = 148 + 6; // strip + gutter, as layoutChannelStrips
    constexpr int stripInset = 4;
    const auto local = stripContent.getLocalPoint(nullptr, screenPosition);
    // Nearest boundary between strips: 0 = before the first, count = after last.
    const int index = juce::jlimit(0, count, (local.x - stripInset + stripWidth / 2) / stripWidth);
    channelInsertIndex = index;
    stripContent.dropMarkerX = insertionBarX(index, stripInset, stripWidth, stripWidth - 148);
    stripContent.repaint();
}

void MixerComponent::finishChannelDrag(std::uint32_t channelId, int sourceIndex) {
    const int insertion = channelInsertIndex;
    channelInsertIndex = -1;
    stripContent.dropMarkerX = -1.0f;
    stripContent.repaint();
    if (insertion < 0) return;
    const int target = moveTargetIndex(insertion, sourceIndex, strips.size());
    if (target < 0 || target == sourceIndex) return;
    // Deferred past this mouse-up: the reorder rebuilds the strips, including
    // the one whose handler is still on the stack.
    juce::Component::SafePointer<MixerComponent> safe(this);
    juce::MessageManager::callAsync([safe, channelId, target] {
        if (safe == nullptr) return;
        safe->audioEngine.moveChannel(channelId, target);
        safe->rebuild();
        if (safe->onStructureChanged) safe->onStructureChanged();
    });
}

void MixerComponent::updateOutputDrag(juce::Point<int> screenPosition) {
    const int count = outputStrips.size();
    if (count <= 0) return;
    constexpr int outWidth = 148 + 6; // strip + gutter, as layoutOutputStrips
    constexpr int outInset = 4;
    const auto local = masterContent.getLocalPoint(nullptr, screenPosition);
    const int index = juce::jlimit(0, count, (local.x - outInset + outWidth / 2) / outWidth);
    outputInsertIndex = index;
    masterContent.dropMarkerX = insertionBarX(index, outInset, outWidth, outWidth - 148);
    masterContent.repaint();
}

void MixerComponent::finishOutputDrag(std::uint32_t outputId, int sourceIndex) {
    const int insertion = outputInsertIndex;
    outputInsertIndex = -1;
    masterContent.dropMarkerX = -1.0f;
    masterContent.repaint();
    if (insertion < 0) return;
    const int target = moveTargetIndex(insertion, sourceIndex, outputStrips.size());
    if (target < 0 || target == sourceIndex) return;
    juce::Component::SafePointer<MixerComponent> safe(this);
    juce::MessageManager::callAsync([safe, outputId, target] {
        if (safe == nullptr) return;
        safe->audioEngine.moveOutput(outputId, target);
        safe->rebuild();
        if (safe->onStructureChanged) safe->onStructureChanged();
    });
}

void MixerComponent::refreshStrips() {
    for (auto* s : strips) {
        if (s != nullptr) {
            s->refreshInput();
            s->refreshInserts();
        }
    }
    for (auto* s : outputStrips) {
        if (s != nullptr) {
            s->refresh();
            s->refreshInserts();
        }
    }
}

void MixerComponent::rebuildChannelStrips() {
    int stripIndex = 0;
    for (const auto& ch : audioEngine.getChannels()) {
        auto* s = strips.add(new ChannelStrip(*ch, audioEngine));
        const std::uint32_t id = ch->id;
        const int sourceIndex = stripIndex++;
        s->onRemoveRequested = [this, id] {
            // The rack bay may describe this strip. Release-only on purpose (see
            // InsertHost.h): a rebuild here would re-describe a doomed insert.
            if (auto release = pluginRemovingHook()) release();
            const auto err = audioEngine.removeChannel(id);
            if (err.isNotEmpty()) {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
                return;
            }
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
        // Every board reorders from the same grip, including the I/O tabs.
        s->setDragHandleVisible(true);
        s->onDragHandleStarted = [this] { channelInsertIndex = -1; };
        s->onDragHandleMoved = [this](juce::Point<int> screenPosition) {
            updateChannelDrag(screenPosition);
        };
        s->onDragHandleFinished = [this, id, sourceIndex] { finishChannelDrag(id, sourceIndex); };
        stripContent.addAndMakeVisible(*s);
    }
}

void MixerComponent::rebuildOutputStrips() {
    int outputIndex = 0;
    for (const auto& out : audioEngine.getOutputs()) {
        auto* s = outputStrips.add(new OutputStrip(*out, outputIndex++, audioEngine));
        const std::uint32_t id = out->id;
        s->onRemoveRequested = [this, id] {
            // The rack bay may describe this strip. Release-only on purpose (see
            // InsertHost.h): a rebuild here would re-describe a doomed insert.
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
        // Output strips reorder with the same grip as the input strips.
        const int sourceIndex = outputIndex - 1;
        s->setDragHandleVisible(true);
        s->onDragHandleStarted = [this] { outputInsertIndex = -1; };
        s->onDragHandleMoved = [this](juce::Point<int> screenPosition) {
            updateOutputDrag(screenPosition);
        };
        s->onDragHandleFinished = [this, id, sourceIndex] { finishOutputDrag(id, sourceIndex); };
        masterContent.addAndMakeVisible(*s);
    }
}
