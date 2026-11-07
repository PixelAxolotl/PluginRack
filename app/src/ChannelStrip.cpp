#include "ChannelStrip.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kStripWidth = 148;
}

ChannelStrip::ChannelStrip(AudioEngine::Channel& channelState, AudioEngine& engine)
    : channel(channelState),
      audioEngine(engine),
      levelMeter(faderSlider, channelState.peakL, channelState.peakR, channelState.rmsL,
                 channelState.rmsR) {
    deviceControl.setTitle(channel.name);
    refreshInput();

    // The strip header carries the name and the device; the input chip below
    // states which physical inputs it listens to and turns into the mapping
    // controls when clicked.
    inputChip.setColour(juce::TextButton::buttonColourId, theme::control());
    inputChip.setColour(juce::TextButton::buttonOnColourId, theme::line());
    inputChip.setColour(juce::TextButton::textColourOffId, theme::text());
    inputChip.setColour(juce::TextButton::textColourOnId, theme::text());
    inputChip.setClickingTogglesState(true);
    inputChip.setTooltip("Input channels - click to change");
    inputChip.onClick = [this] {
        inputEditing = inputChip.getToggleState();
        refreshInput();
    };
    addAndMakeVisible(inputChip);

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
        const auto current = audioEngine.getChannelDeviceState(channel.id);
        DeviceMenu::showInputMenu(deviceControl, current.deviceName,
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
        juce::Component::SafePointer<ChannelStrip> safe(this);
        StripDialog::show("Rename channel", channel.name, channel.colourHue,
                          [safe](const juce::String& newName, float newHue) {
            if (safe == nullptr) return;
            safe->audioEngine.setChannelName(safe->channel.id, newName);
            safe->audioEngine.setChannelColourHue(safe->channel.id, newHue);
            safe->deviceControl.setTitle(newName);
            safe->repaint();
            if (safe->rebuildCallback) safe->rebuildCallback();
        });
    });
    addAndMakeVisible(deviceControl);

    modeCombo.addItem("MONO", 1);
    modeCombo.addItem("ST", 2);
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

    vstHeader.setJustificationType(juce::Justification::centredLeft);
    vstHeader.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    vstHeader.setColour(juce::Label::textColourId, theme::violet());
    addAndMakeVisible(vstHeader);

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
    levelKnob.setName("Trim");

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
    panKnob.setName("Pan");

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
    // Fader in the channel's own identity colour; the bottom accent bar is
    // painted separately in ChannelStrip::paint() and stays as is.
    const auto channelAccent = theme::stripColour(channel.colourHue);
    faderSlider.setColour(juce::Slider::trackColourId, channelAccent);
    faderSlider.setColour(juce::Slider::backgroundColourId, theme::strip());
    faderSlider.setColour(juce::Slider::thumbColourId, channelAccent);
    faderSlider.setColour(juce::Slider::textBoxTextColourId, theme::text());
    faderSlider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    faderSlider.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    addAndMakeVisible(faderSlider);
    faderSlider.setName("Gain");

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
    if (multibandWindow != nullptr) {
        delete multibandWindow;
        multibandWindow = nullptr;
    }
    if (deEsserWindow != nullptr) {
        delete deEsserWindow;
        deEsserWindow = nullptr;
    }
    if (limiterWindow != nullptr) {
        delete limiterWindow;
        limiterWindow = nullptr;
    }
}
// openers appended below
void ChannelStrip::openMultibandWindow() {
    if (multibandWindow != nullptr) { multibandWindow->toFront(true); return; }
    if (channel.multibandEffects.empty()) {
        auto effects = channel.multibandEffects;
        effects.push_back(pluginrack::makeDefaultMultibandEffect(1));
        audioEngine.setChannelMultiband(channel.id, effects);
        audioEngine.ensureChannelChainItem(channel.id, {pluginrack::ChainItemType::Multiband, 1});
    }
    juce::Component::SafePointer<ChannelStrip> safe(this);
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
    multibandWindow = new MultibandWindow("Multiband Compressor - " + channel.name, channel.multibandEffects.front(),
        [safe](const pluginrack::MultibandEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto effects = safe->channel.multibandEffects;
            for (auto& e : effects) if (e.id == effect.id) { const bool bp = e.bypassed; e = effect; if (!bypassChanged) e.bypassed = bp; }
            safe->audioEngine.setChannelMultiband(safe->channel.id, effects);
            safe->refreshInserts();
        }, channel.multibandRtaOffsetDb, channel.multibandRtaWeighting, liveRateHz);
    multibandWindow->applySpectrumOffset = [safe](float offsetDb) {
        if (safe != nullptr) safe->audioEngine.setChannelMultibandRtaOffset(safe->channel.id, offsetDb);
    };
    multibandWindow->applyWeighting = [safe](int weighting) {
        if (safe != nullptr) safe->audioEngine.setChannelMultibandRtaWeighting(safe->channel.id, weighting);
    };
    multibandWindow->onClose = [safe] { if (safe != nullptr && safe->multibandWindow) { auto* w = safe->multibandWindow; safe->multibandWindow = nullptr; delete w; } };
    multibandWindow->pullAnalyzerTap = [safe](float* l, float* r, int n) { if (safe != nullptr) safe->channel.multibandTap.snapshot(true, l, r, n); };
    channel.multibandTap.setMode(AudioEngine::AnalyzerTap::kPost);
}
void ChannelStrip::openDeEsserWindow() {
    if (deEsserWindow != nullptr) { deEsserWindow->toFront(true); return; }
    if (channel.deEsserEffects.empty()) {
        auto effects = channel.deEsserEffects;
        effects.push_back(pluginrack::makeDefaultDeEsserEffect(1));
        audioEngine.setChannelDeEssers(channel.id, effects);
        audioEngine.ensureChannelChainItem(channel.id, {pluginrack::ChainItemType::DeEsser, 1});
    }
    juce::Component::SafePointer<ChannelStrip> safe(this);
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
    deEsserWindow = new DeEsserWindow("De-esser - " + channel.name, channel.deEsserEffects.front(),
        [safe](const pluginrack::DeEsserEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto effects = safe->channel.deEsserEffects;
            for (auto& e : effects) if (e.id == effect.id) { const bool bp = e.bypassed; e = effect; if (!bypassChanged) e.bypassed = bp; }
            safe->audioEngine.setChannelDeEssers(safe->channel.id, effects);
            safe->refreshInserts();
        }, channel.deEsserRtaOffsetDb, channel.deEsserRtaWeighting, liveRateHz);
    deEsserWindow->applySpectrumOffset = [safe](float offsetDb) {
        if (safe != nullptr) safe->audioEngine.setChannelDeEsserRtaOffset(safe->channel.id, offsetDb);
    };
    deEsserWindow->applyWeighting = [safe](int weighting) {
        if (safe != nullptr) safe->audioEngine.setChannelDeEsserRtaWeighting(safe->channel.id, weighting);
    };
    deEsserWindow->onClose = [safe] { if (safe != nullptr && safe->deEsserWindow) { auto* w = safe->deEsserWindow; safe->deEsserWindow = nullptr; delete w; } };
    deEsserWindow->pullAnalyzerTap = [safe](float* l, float* r, int n) { if (safe != nullptr) safe->channel.deEsserTap.snapshot(true, l, r, n); };
    channel.deEsserTap.setMode(AudioEngine::AnalyzerTap::kPost);
}
void ChannelStrip::openLimiterWindow() {
    if (limiterWindow != nullptr) { limiterWindow->toFront(true); return; }
    if (channel.limiterEffects.empty()) {
        auto effects = channel.limiterEffects;
        effects.push_back(pluginrack::makeDefaultLimiterEffect(1));
        audioEngine.setChannelLimiters(channel.id, effects);
        audioEngine.ensureChannelChainItem(channel.id, {pluginrack::ChainItemType::Limiter, 1});
    }
    juce::Component::SafePointer<ChannelStrip> safe(this);
    limiterWindow = new LimiterWindow("Limiter - " + channel.name, channel.limiterEffects.front(),
        [safe](const pluginrack::LimiterEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto effects = safe->channel.limiterEffects;
            for (auto& e : effects) if (e.id == effect.id) { const bool bp = e.bypassed; e = effect; if (!bypassChanged) e.bypassed = bp; }
            safe->audioEngine.setChannelLimiters(safe->channel.id, effects);
            safe->refreshInserts();
        });
    limiterWindow->onClose = [safe] { if (safe != nullptr && safe->limiterWindow) { auto* w = safe->limiterWindow; safe->limiterWindow = nullptr; delete w; } };
    limiterWindow->pullAnalyzerTap = [safe](bool /*postEq*/, float* l, float* r, int n) {
        if (safe != nullptr) safe->channel.limiterTap.snapshot(true, l, r, n);
    };
    channel.limiterTap.setMode(AudioEngine::AnalyzerTap::kPost);
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
            safe->channel.dynamicsTap.setMode(AudioEngine::AnalyzerTap::kOff);
            safe->channel.analyzerTap.setMode(AudioEngine::AnalyzerTap::kOff);
            auto* window = safe->dynamicsWindow;
            safe->dynamicsWindow = nullptr;
            delete window;
        }
    };
    // Same method as every other RTA; its own tap so the compressor's
    // output is what its graph shows, not the strip end or a later plugin.
    dynamicsWindow->pullAnalyzerTap = [safe](float* left, float* right, int frames) {
        if (safe != nullptr) {
            safe->channel.dynamicsTap.setMode(AudioEngine::AnalyzerTap::kPost);
            safe->channel.dynamicsTap.snapshot(true, left, right, frames);
        }
    };
    channel.dynamicsTap.setMode(AudioEngine::AnalyzerTap::kPost);
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
        liveRateHz, false, channel.eqRtaOffsetDb, channel.eqRtaWeighting);
    window->applySpectrumOffset = [safe](float offsetDb) {
        if (safe == nullptr) return;
        safe->audioEngine.setChannelEqRtaOffset(safe->channel.id, offsetDb);
    };
    window->applyWeighting = [safe](int weighting) {
        if (safe == nullptr) return;
        safe->audioEngine.setChannelEqRtaWeighting(safe->channel.id, weighting);
    };
    // Analyzer wiring: the EQ reads its own post-EQ tap (eqTap), so its
    // RTA shows what leaves the EQ — a later Multiband can't tint it, and
    // the Multiband RTA (multibandTap) can't see the EQ's boost.
    window->pullAnalyzerTap = [safe](bool postEq, float* destL, float* destR) {
        if (safe == nullptr) return;
        safe->channel.eqTap.snapshot(postEq, destL, destR,
                                     EQWindow::kAnalyzerFftSize);
    };
    window->setAnalyzerTapMode = [safe](int mode) {
        if (safe == nullptr) return;
        safe->channel.eqTap.setMode(static_cast<std::uint32_t>(mode));
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
    // Phase flip: seeded from the strip, persisted through the engine.
    // Editing engages the EQ like a band drag does.
    window->setPhaseFlip(channel.eqPhaseFlip.get());
    window->applyPhaseFlip = [safe](bool flipped) {
        if (safe == nullptr) return;
        safe->audioEngine.setChannelEqBypassed(safe->channel.id, false);
        safe->audioEngine.setChannelEqPhaseFlip(safe->channel.id, flipped);
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

    // Header: the strip's own buttons take a short row of their own so the
    // channel name keeps the full width instead of collapsing to "Cha...".
    auto actionsRow = area.removeFromTop(theme::metrics::kActionH);
    // The grip takes its lane before any action does: a button that eats the
    // row would leave it zero-wide and make the strip impossible to drag.
    dragHandleBounds = dragHandleVisible
                           ? actionsRow.removeFromLeft(theme::metrics::kActionH)
                           : juce::Rectangle<int>();
    removeChannelButton.setBounds(actionsRow.removeFromRight(theme::metrics::kCloseW));
    actionsRow.removeFromRight(2);
    splitButton.setBounds(actionsRow.removeFromRight(theme::metrics::kActionW));
    area.removeFromTop(2);
    deviceControl.setBounds(area.removeFromTop(theme::metrics::kDeviceH));
    area.removeFromTop(4);

    // Input chip, or the mapping controls it swaps into when clicked.
    auto inputRow = area.removeFromTop(theme::metrics::kFieldH);
    if (inputEditing) {
        modeCombo.setBounds(inputRow.removeFromLeft(44));
        inputRow.removeFromLeft(2);
        inputComboA.setBounds(inputRow.removeFromLeft(36));
        inputRow.removeFromLeft(2);
        if (inputComboB.isVisible()) {
            inputComboB.setBounds(inputRow.removeFromLeft(36));
            inputRow.removeFromLeft(2);
        }
        inputChip.setBounds(inputRow);
    } else {
        inputChip.setBounds(inputRow);
    }
    area.removeFromTop(2);

    gateButton.setVisible(false);
    eqButton.setVisible(false);
    compressorButton.setVisible(false);

    // Trim and Pan share one row, above the chain: the first two things a
    // channel is reached for sit where the eye lands, not below eight slots.
    auto gainRow = area.removeFromTop(74);
    const int knobCellW = (gainRow.getWidth() - 4) / 2;
    auto trimCell = gainRow.removeFromLeft(knobCellW);
    gainRow.removeFromLeft(4);
    auto panCell = gainRow.removeFromLeft(knobCellW);
    levelLabel.setBounds(trimCell.removeFromTop(14));
    levelKnob.setBounds(trimCell.reduced(2));
    panLabel.setBounds(panCell.removeFromTop(14));
    panKnob.setBounds(panCell.reduced(2));
    area.removeFromTop(4);

    // Unified built-in and VST chain: 3 built-ins + 5 VST slots, always.
    // The rows give height back before they squeeze the fader out of the strip.
    vstHeader.setBounds(area.removeFromTop(14));
    const int routingRows = juce::jmax(1, ((int)routingButtons.size() + 3) / 4);
    const int reservedBelow = routingRows * 20 + 4 + 150;  // routing + fader
    const int rowCount = juce::jmax(1, slots.size());
    const int chainSpace = juce::jmax(0, area.getHeight() - 4 - reservedBelow);
    const int slotH = juce::jlimit(14, 20, (chainSpace - 2 * rowCount) / rowCount);
    for (int i = 0; i < (int)slots.size(); ++i) {
        slots[i]->setBounds(area.removeFromTop(slotH));
        area.removeFromTop(2);
    }
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

    // fader + meter (bottom, dominant). The meter lines its bars up with the
    // fader's travel itself and keeps the readouts in the strip above the
    // travel, so it takes the fader's full height.
    auto footer = area.removeFromBottom(theme::metrics::kRowH); // M / S
    const int half = (footer.getWidth() - 4) / 2;
    auto fm = area;
    auto meterColumn = fm.removeFromRight(LevelMeter::kPreferredWidth);
    fm.removeFromRight(4);
    faderSlider.setBounds(fm);
    levelMeter.setBounds(meterColumn);

    muteButton.setBounds(footer.removeFromLeft(half));
    footer.removeFromLeft(4);
    soloButton.setBounds(footer);
}

void ChannelStrip::refreshInserts() {
    // The compressor and the gate share the Dynamics card, so rows are not a
    // one-to-one map of the chain any more.
    const auto rows = pluginrack::buildChainRows(channel.chain, channel.effects);
    chainRowCount = static_cast<int>(rows.size());
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

    const auto compressor = std::find_if(channel.effects.begin(), channel.effects.end(),
                                         [](const auto& effect) {
                                             return effect.type
                                                    == pluginrack::DynamicsEffectType::Compressor;
                                         });
    const auto gate = std::find_if(channel.effects.begin(), channel.effects.end(),
                                   [](const auto& effect) {
                                       return effect.type == pluginrack::DynamicsEffectType::Gate;
                                   });
    for (int r = 0; r < chainRowCount; ++r) {
        const auto& indices = rows[static_cast<std::size_t>(r)];
        const auto item = channel.chain[static_cast<std::size_t>(indices.front())];
        if (indices.size() > 1) {
            // Compressor + gate: one switch each, and a click on the row opens
            // the panel that carries both.
            const int compIndex =
                pluginrack::compressorIndexInRow(channel.chain, channel.effects, indices);
            const int gateIndex = indices.front() == compIndex ? indices.back() : indices.front();
            const auto compItem = channel.chain[static_cast<std::size_t>(compIndex)];
            const auto gateItem = channel.chain[static_cast<std::size_t>(gateIndex)];
            slots[r]->setBuiltInPair(
                "Dynamics", compItem,
                CardSwitch{ compItem, compressor != channel.effects.end() && compressor->bypassed,
                            "C", "Compressor: click to bypass, right-click to open the panel" },
                CardSwitch{ gateItem, gate != channel.effects.end() && gate->bypassed, "G",
                            "Gate: click to bypass, right-click to open the panel" });
            continue;
        }
        if (item.type == pluginrack::ChainItemType::Insert) {
            auto it = std::find_if(channel.inserts.begin(), channel.inserts.end(),
                                   [id = item.id](const auto& insert) { return insert->id == id; });
            slots[r]->setInsert(it == channel.inserts.end() ? nullptr : it->get());
        } else if (item.type == pluginrack::ChainItemType::Eq) {
            slots[r]->setBuiltIn(item, "EQ", channel.eqBypassed.get());
        } else if (item.type == pluginrack::ChainItemType::Multiband) {
            const auto it = std::find_if(channel.multibandEffects.begin(), channel.multibandEffects.end(), [id=item.id](const auto& e){return e.id==id;});
            slots[r]->setBuiltIn(item, "Multiband Comp", it!=channel.multibandEffects.end() && it->bypassed);
        } else if (item.type == pluginrack::ChainItemType::DeEsser) {
            const auto it = std::find_if(channel.deEsserEffects.begin(), channel.deEsserEffects.end(), [id=item.id](const auto& e){return e.id==id;});
            slots[r]->setBuiltIn(item, "De-esser", it!=channel.deEsserEffects.end() && it->bypassed);
        } else if (item.type == pluginrack::ChainItemType::Limiter) {
            const auto it = std::find_if(channel.limiterEffects.begin(), channel.limiterEffects.end(), [id=item.id](const auto& e){return e.id==id;});
            slots[r]->setBuiltIn(item, "Limiter", it!=channel.limiterEffects.end() && it->bypassed);
        } else {
            const auto it = std::find_if(channel.effects.begin(), channel.effects.end(),
                                         [id = item.id](const auto& effect) { return effect.id == id; });
            const bool isCompressor = it != channel.effects.end()
                                          ? it->type == pluginrack::DynamicsEffectType::Compressor
                                          : item.id == 1;
            slots[r]->setBuiltIn(item, isCompressor ? "Compressor" : "Gate",
                                 it != channel.effects.end() && it->bypassed);
        }
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

void ChannelStrip::rebuildInputCombos(int count) {
    if (count == inputComboCount) return;
    inputComboCount = count;
    inputComboA.clear(juce::dontSendNotification);
    inputComboB.clear(juce::dontSendNotification);
    for (int i = 1; i <= count; ++i) {
        inputComboA.addItem("A" + juce::String(i), i);
        inputComboB.addItem("B" + juce::String(i), i);
    }
}

void ChannelStrip::refreshInput() {
    // Device name/error are written by the device-opener worker: take a locked
    // snapshot instead of reading the strip fields directly.
    const auto deviceState = audioEngine.getChannelDeviceState(channel.id);
    const bool mono = channel.layout == pluginrack::ChannelLayout::Mono;
    const bool hasInputDevice = deviceState.deviceName.isNotEmpty();
    levelMeter.setStereo(!mono);
    modeCombo.setSelectedId(mono ? 1 : 2, juce::dontSendNotification);
    const int a = channel.inputChannels.empty() ? 1 : channel.inputChannels[0] + 1;
    const int b = channel.inputChannels.size() > 1 ? channel.inputChannels[1] + 1 : a;
    // The device decides how many inputs exist; the 16 floor keeps the control
    // usable before a device is open, and the mapping is always allowed so a
    // saved index above the device's count is shown, never silently moved.
    rebuildInputCombos(juce::jmax(16, deviceState.availableInputs, a, b));
    inputComboA.setSelectedId(a, juce::dontSendNotification);
    inputComboB.setSelectedId(b, juce::dontSendNotification);
    const juce::String aText = "A" + juce::String(a);
    if (!hasInputDevice) inputChip.setButtonText("No input");
    else if (inputEditing) inputChip.setButtonText("Done");
    else if (mono) inputChip.setButtonText(aText);
    else inputChip.setButtonText(aText + " + A" + juce::String(b));
    inputChip.setToggleState(inputEditing, juce::dontSendNotification);
    modeCombo.setVisible(hasInputDevice && inputEditing);
    inputComboA.setVisible(hasInputDevice && inputEditing);
    inputComboB.setVisible(hasInputDevice && inputEditing && !mono);
    deviceControl.setDeviceName(deviceState.deviceName);
    deviceControl.setBackendType(deviceState.deviceType);
    // The device control already shows the error in red (and its tooltip), so
    // it is not repeated a second time under the input chip.
    deviceControl.setError(deviceState.error);
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
    const auto deviceState = audioEngine.getChannelDeviceState(channel.id);
    const auto& device = deviceState.deviceName;
    // The capture only stores what was requested; the device may expose more
    // physical inputs than this channel's mapping uses.
    const int inputs = deviceState.availableInputs;
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

juce::String ChannelStrip::insertPlugin(const juce::File& file, int pluginUniqueId) {
    // The load is asynchronous; the strip refreshes when it finishes.
    const auto channelId = channel.id;
    juce::Component::SafePointer<ChannelStrip> safe(this);
    audioEngine.addPluginAsync(channelId, file, pluginUniqueId, [safe](juce::String error) {
        if (safe == nullptr) return;
        safe->refreshAfterInsertChange();
        if (error.isNotEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "PluginRack", error);
        }
    });
    return {};
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

void ChannelStrip::reorderInsert(std::uint32_t insertId, int targetIndex) {
    audioEngine.moveChannelChainItem(channel.id, {pluginrack::ChainItemType::Insert, insertId},
                                     targetIndex);
    refreshAfterInsertChange();
}

void ChannelStrip::reorderChainItem(pluginrack::ChainItem item, int targetIndex) {
    // A card moves as one: collect every chain entry it covers.
    const auto rows = pluginrack::buildChainRows(channel.chain, channel.effects);
    std::vector<pluginrack::ChainItem> cardItems{ item };
    for (const auto& row : rows) {
        const bool holdsItem = std::any_of(row.begin(), row.end(), [this, &item](int index) {
            return channel.chain[static_cast<std::size_t>(index)] == item;
        });
        if (!holdsItem) continue;
        if (row.size() > 1) {
            cardItems.clear();
            for (const int index : row)
                cardItems.push_back(channel.chain[static_cast<std::size_t>(index)]);
        }
        break;
    }
    // Plan the card against a simulated chain: the engine erases each item
    // before inserting it, so a drop below the card lands where the marker
    // showed instead of one row past it.
    const auto moves =
        pluginrack::planChainCardMoves(channel.chain, rows, targetIndex, cardItems);
    for (const auto& move : moves)
        audioEngine.moveChannelChainItem(channel.id, move.item, move.targetIndex);
    juce::Component::SafePointer<ChannelStrip> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        // Copy the callback: the rebuild destroys this strip.
        auto callback = safe->rebuildCallback;
        safe->chainDropTarget = -1;
        safe->refreshInserts();
        if (callback) callback();
    });
}

int ChannelStrip::chainBoundaryY(int targetIndex) const {
    if (chainRowCount <= 0) return vstHeader.getBottom() + 1;
    const int target = juce::jlimit(0, chainRowCount, targetIndex);
    if (target < chainRowCount) return juce::jmax(0, slots[target]->getY() - 2);
    return slots[chainRowCount - 1]->getBottom();
}

int ChannelStrip::chainDropIndexAt(juce::Point<int> parentPoint) {
    int index = 0;
    for (int i = 0; i < chainRowCount; ++i)
        if (parentPoint.y > slots[i]->getBounds().getCentreY()) index = i + 1;
    return index;
}

void ChannelStrip::previewChainItem(int targetIndex) {
    chainDropTarget = juce::jlimit(0, chainRowCount, targetIndex);
    repaint();
}

void ChannelStrip::clearChainPreview() {
    chainDropTarget = -1;
    repaint();
}

void ChannelStrip::setBuiltInBypass(pluginrack::ChainItem item, bool bypassed) {
    if (item.type == pluginrack::ChainItemType::Eq) {
        audioEngine.setChannelEqBypassed(channel.id, bypassed);
        return;
    }
    if (item.type == pluginrack::ChainItemType::Multiband) {
        auto effects = channel.multibandEffects;
        for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
        audioEngine.setChannelMultiband(channel.id, effects);
        if (multibandWindow != nullptr) multibandWindow->setBypassed(bypassed);
        return;
    }
    if (item.type == pluginrack::ChainItemType::DeEsser) {
        auto effects = channel.deEsserEffects;
        for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
        audioEngine.setChannelDeEssers(channel.id, effects);
        if (deEsserWindow != nullptr) deEsserWindow->setBypassed(bypassed);
        return;
    }
    if (item.type == pluginrack::ChainItemType::Limiter) {
        auto effects = channel.limiterEffects;
        for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
        audioEngine.setChannelLimiters(channel.id, effects);
        if (limiterWindow != nullptr) limiterWindow->setBypassed(bypassed);
        return;
    }
    auto effects = channel.effects;
    for (auto& effect : effects)
        if (effect.id == item.id) effect.bypassed = bypassed;
    audioEngine.setChannelEffects(channel.id, effects);
    // An open dynamics panel mirrors the gate row; a chain-row toggle has to
    // reach it or the panel keeps showing the old switch state.
    refreshDynamicsButtons();
}

void ChannelStrip::activateBuiltIn(pluginrack::ChainItem item) {
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

void ChannelStrip::insertBuiltIn(pluginrack::ChainItem item, int chainIndex) {
    // Built-ins are singletons: picking one from the menu moves it to the
    // clicked slot and unbypasses it, so the effect audibly lands there.
    // A removed built-in is not in the chain yet, so restore it first — the
    // move planner only relocates entries that exist.
    audioEngine.ensureChannelChainItem(channel.id, item);
    if (item.type == pluginrack::ChainItemType::Multiband && std::none_of(channel.multibandEffects.begin(), channel.multibandEffects.end(), [&item](const auto& e){return e.id==item.id;})) {
        auto effects = channel.multibandEffects; effects.push_back(pluginrack::makeDefaultMultibandEffect(item.id)); audioEngine.setChannelMultiband(channel.id, effects);
    }
    if (item.type == pluginrack::ChainItemType::DeEsser && std::none_of(channel.deEsserEffects.begin(), channel.deEsserEffects.end(), [&item](const auto& e){return e.id==item.id;})) {
        auto effects = channel.deEsserEffects; effects.push_back(pluginrack::makeDefaultDeEsserEffect(item.id)); audioEngine.setChannelDeEssers(channel.id, effects);
    }
    if (item.type == pluginrack::ChainItemType::Limiter && std::none_of(channel.limiterEffects.begin(), channel.limiterEffects.end(), [&item](const auto& e){return e.id==item.id;})) {
        auto effects = channel.limiterEffects; effects.push_back(pluginrack::makeDefaultLimiterEffect(item.id)); audioEngine.setChannelLimiters(channel.id, effects);
    }
    // Comp/Gate is one object: picking it restores the partner row too, so
    // the card comes back whole instead of one effect at a time.
    if (item.type == pluginrack::ChainItemType::Dynamics) {
        for (const auto& effect : channel.effects)
            if (effect.id != item.id)
                audioEngine.ensureChannelChainItem(channel.id,
                                                   {pluginrack::ChainItemType::Dynamics, effect.id});
    }
    const auto rows = pluginrack::buildChainRows(channel.chain, channel.effects);
    std::vector<pluginrack::ChainItem> cardItems{ item };
    for (const auto& row : rows) {
        const bool holdsItem = std::any_of(row.begin(), row.end(), [this, &item](int index) {
            return channel.chain[static_cast<std::size_t>(index)] == item;
        });
        if (!holdsItem) continue;
        if (row.size() > 1) {
            cardItems.clear();
            for (const int index : row)
                cardItems.push_back(channel.chain[static_cast<std::size_t>(index)]);
        }
        break;
    }
    const auto moves =
        pluginrack::planChainCardMoves(channel.chain, rows, chainIndex, cardItems);
    for (const auto& move : moves)
        audioEngine.moveChannelChainItem(channel.id, move.item, move.targetIndex);
    setBuiltInBypass(item, false);
    refreshAfterInsertChange();
}

void ChannelStrip::eraseBuiltIn(pluginrack::ChainItem item) {
    // Dynamics is one node: removing the card takes both effects.
    std::vector<pluginrack::ChainItem> cardItems{ item };
    if (item.type == pluginrack::ChainItemType::Dynamics) {
        const auto rows = pluginrack::buildChainRows(channel.chain, channel.effects);
        for (const auto& row : rows) {
            const bool holdsItem =
                std::any_of(row.begin(), row.end(), [this, &item](int index) {
                    return channel.chain[static_cast<std::size_t>(index)] == item;
                });
            if (!holdsItem || row.size() <= 1) continue;
            cardItems.clear();
            for (const int index : row)
                cardItems.push_back(channel.chain[static_cast<std::size_t>(index)]);
            break;
        }
    }
    for (const auto& entry : cardItems)
        audioEngine.removeChannelChainItem(channel.id, entry);
    refreshAfterInsertChange();
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
        // Active chip lights in the output's own identity colour.
        btn->setColour(juce::TextButton::buttonOnColourId, theme::stripColour(output->colourHue));
        btn->setColour(juce::TextButton::textColourOffId, theme::text());
        btn->setColour(juce::TextButton::textColourOnId, juce::Colour(0xff0a0c0e));

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
    theme::drawStripFace(g, getLocalBounds());
    auto content = getLocalBounds().reduced(1);
    g.setColour(theme::panel().withAlpha(0.42f));
    g.fillRect(content.removeFromTop(24));
    // Identity colour: bottom accent bar in the strip's own hue, inset for
    // the rounded corners so it never clips under the buttons above.
    theme::drawStripAccent(g, getLocalBounds(), channel.colourHue);
    paintDragHandle(g);
    if (chainDropTarget >= 0) {
        g.setColour(theme::cyan());
        g.fillRoundedRectangle(4.0f, static_cast<float>(chainBoundaryY(chainDropTarget)),
                               static_cast<float>(juce::jmax(0, getWidth() - 8)), 2.0f, 1.0f);
    }
}

void ChannelStrip::setDragHandleVisible(bool visible) {
    if (dragHandleVisible == visible) return;
    dragHandleVisible = visible;
    draggingHandle = false;
    resized();
    repaint();
}

void ChannelStrip::paintDragHandle(juce::Graphics& g) const {
    if (!dragHandleVisible || dragHandleBounds.isEmpty()) return;
    g.setColour(draggingHandle ? theme::cyan() : theme::dim().withAlpha(0.65f));
    const auto centre = dragHandleBounds.toFloat().getCentre();
    // Two columns of three dots: the usual "grab me to reorder" grip.
    for (int row = -1; row <= 1; ++row) {
        for (int column = 0; column <= 1; ++column) {
            g.fillEllipse(centre.x - 2.5f + static_cast<float>(column) * 4.0f,
                          centre.y - 3.0f + static_cast<float>(row) * 3.0f, 2.0f, 2.0f);
        }
    }
}

void ChannelStrip::mouseDown(const juce::MouseEvent& e) {
    if (onSelected) onSelected();
    if (dragHandleVisible && e.mods.isLeftButtonDown()
        && dragHandleBounds.contains(e.getPosition())) {
        draggingHandle = true;
        repaint();
        if (onDragHandleStarted) onDragHandleStarted();
    }
}

void ChannelStrip::mouseDrag(const juce::MouseEvent& e) {
    if (!draggingHandle) return;
    if (onDragHandleMoved) onDragHandleMoved(e.getScreenPosition());
}

void ChannelStrip::mouseUp(const juce::MouseEvent&) {
    if (!draggingHandle) return;
    draggingHandle = false;
    repaint();
    if (onDragHandleFinished) onDragHandleFinished();
}

void ChannelStrip::mouseMove(const juce::MouseEvent& e) {
    setMouseCursor(dragHandleVisible && dragHandleBounds.contains(e.getPosition())
                       ? juce::MouseCursor::DraggingHandCursor
                       : juce::MouseCursor::NormalCursor);
}

void ChannelStrip::mouseDoubleClick(const juce::MouseEvent&) {
    if (onDoubleClicked) onDoubleClicked();
}
