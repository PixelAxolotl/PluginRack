#include "RackView.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "NativeThumbnail.h"

namespace {
constexpr int kSlotGap = 4;
constexpr int kSlotInset = 4;
// Newest samples the dynamics thumbnails measure their level over; same
// window the dynamics panel uses.
constexpr int kLevelFrames = 512;
} // namespace

RackView::RackView(AudioEngine& engine) : audioEngine(engine) {
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    nameLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    nameLabel.setColour(juce::Label::textColourId, theme::text());
    nameLabel.setInterceptsMouseClicks(false, false);
    nameLabel.setTooltip("Right-click to rename");
    addAndMakeVisible(nameLabel);

    subLabel.setJustificationType(juce::Justification::centredLeft);
    subLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    subLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(subLabel);
    outputHeader.setColour(juce::Label::textColourId, theme::amber());

    gainCaption.setText("Gain", juce::dontSendNotification);
    gainCaption.setJustificationType(juce::Justification::centred);
    gainCaption.setFont(juce::Font(juce::FontOptions(10.0f)));
    gainCaption.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(gainCaption);

    gainSlider.setSliderStyle(juce::Slider::LinearVertical);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxAbove, false, 64, 20);
    gainSlider.setRange(-60.0, 12.0, 0.1);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setDoubleClickReturnValue(true, 0.0);
    gainSlider.setColour(juce::Slider::trackColourId, theme::deepGreen());
    gainSlider.setColour(juce::Slider::backgroundColourId, theme::strip());
    gainSlider.setColour(juce::Slider::thumbColourId, theme::green());
    gainSlider.setColour(juce::Slider::textBoxTextColourId, theme::text());
    gainSlider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    gainSlider.onValueChange = [this] {
        const float v = static_cast<float>(gainSlider.getValue());
        if (auto* ch = currentChannel()) audioEngine.setChannelGain(ch->id, v);
        else if (auto* out = currentOutput()) audioEngine.setOutputGain(out->id, v);
    };
    addAndMakeVisible(gainSlider);
    gainSlider.setName("Gain");

    trimCaption.setText("Trim", juce::dontSendNotification);
    trimCaption.setJustificationType(juce::Justification::centred);
    trimCaption.setFont(juce::Font(juce::FontOptions(10.0f)));
    trimCaption.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(trimCaption);

    trimKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    trimKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    trimKnob.setRange(-12.0, 12.0, 0.1);
    trimKnob.setTextValueSuffix(" dB");
    trimKnob.setDoubleClickReturnValue(true, 0.0);
    trimKnob.setColour(juce::Slider::rotarySliderFillColourId, theme::green());
    trimKnob.setColour(juce::Slider::rotarySliderOutlineColourId, theme::deepGreen());
    trimKnob.setColour(juce::Slider::textBoxTextColourId, theme::text());
    trimKnob.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    trimKnob.onValueChange = [this] {
        if (auto* ch = currentChannel())
            audioEngine.setChannelTrim(ch->id, static_cast<float>(trimKnob.getValue()));
    };
    addAndMakeVisible(trimKnob);
    trimKnob.setName("Trim");

    panCaption.setText("Pan", juce::dontSendNotification);
    panCaption.setJustificationType(juce::Justification::centred);
    panCaption.setFont(juce::Font(juce::FontOptions(10.0f)));
    panCaption.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(panCaption);

    panSlider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    panSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    panSlider.setRange(-1.0, 1.0, 0.01);
    panSlider.setDoubleClickReturnValue(true, 0.0);
    panSlider.setColour(juce::Slider::rotarySliderFillColourId, theme::cyan());
    panSlider.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
    panSlider.setColour(juce::Slider::textBoxTextColourId, theme::text());
    panSlider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    panSlider.onValueChange = [this] {
        if (auto* ch = currentChannel())
            audioEngine.setChannelPan(ch->id, static_cast<float>(panSlider.getValue()));
    };
    addAndMakeVisible(panSlider);
    panSlider.setName("Pan");

    for (auto* b : {&muteButton, &soloButton}) {
        b->setClickingTogglesState(true);
        addAndMakeVisible(*b);
    }
    muteButton.setColour(juce::TextButton::buttonColourId, theme::control());
    muteButton.setColour(juce::TextButton::buttonOnColourId, theme::red());
    muteButton.setColour(juce::TextButton::textColourOffId, theme::text());
    muteButton.onClick = [this] {
        const bool muted = muteButton.getToggleState();
        if (auto* ch = currentChannel()) audioEngine.setChannelMuted(ch->id, muted);
        else if (auto* out = currentOutput()) audioEngine.setOutputMuted(out->id, muted);
    };
    soloButton.setColour(juce::TextButton::buttonColourId, theme::control());
    soloButton.setColour(juce::TextButton::buttonOnColourId, theme::amber());
    soloButton.setColour(juce::TextButton::textColourOffId, theme::text());
    soloButton.onClick = [this] {
        if (auto* ch = currentChannel()) audioEngine.setChannelSoloed(ch->id, soloButton.getToggleState());
    };

    mixerButton.setColour(juce::TextButton::buttonColourId, theme::control());
    mixerButton.setColour(juce::TextButton::textColourOffId, theme::cyan());
    mixerButton.setTooltip("Show this chain in the Mixer view");
    mixerButton.onClick = [this] { jumpToMixer(); };
    addAndMakeVisible(mixerButton);

    channelCombo.onChange = [this] {
        const int i = channelCombo.getSelectedItemIndex();
        if (i < 0 || i >= (int)comboIds.size()) return;
        const int newId = comboIds[(size_t)i];
        const bool newOut = comboIsOutput[(size_t)i] != 0;
        if (newId == selectedId && newOut == selectedIsOutput) return;
        closeEqWindow();
        closeDynamicsWindow();
        selectedId = newId;
        selectedIsOutput = newOut;
        focusedInsertId = 0;
        inputEditing = false;
        rebuild();
    };
    channelCombo.setLookAndFeel(&selectorLookAndFeel);
    addAndMakeVisible(channelCombo);
    channelCombo.setColour(juce::ComboBox::backgroundColourId, theme::control());
    channelCombo.setColour(juce::ComboBox::textColourId, theme::text());
    channelCombo.setColour(juce::ComboBox::arrowColourId, theme::dim());
    channelCombo.setColour(juce::ComboBox::outlineColourId, theme::border());

    inputChip.setColour(juce::TextButton::buttonColourId, theme::control());
    inputChip.setColour(juce::TextButton::buttonOnColourId, theme::line());
    inputChip.setColour(juce::TextButton::textColourOffId, theme::text());
    inputChip.setColour(juce::TextButton::textColourOnId, theme::text());
    inputChip.setClickingTogglesState(true);
    inputChip.setTooltip("Input channels - click to change");
    inputChip.onClick = [this] {
        inputEditing = inputChip.getToggleState();
        refreshInputControls();
        resized();
    };
    addAndMakeVisible(inputChip);

    modeCombo.addItem("MONO", 1);
    modeCombo.addItem("ST", 2);
    inputComboA.setTooltip("Physical input for this channel (1-based)");
    inputComboB.setTooltip("Physical input for the right side (1-based)");
    const auto styleInputCombo = [this](juce::ComboBox& combo) {
        combo.setColour(juce::ComboBox::backgroundColourId, theme::control());
        combo.setColour(juce::ComboBox::textColourId, theme::text());
        combo.setColour(juce::ComboBox::arrowColourId, theme::dim());
        combo.setColour(juce::ComboBox::outlineColourId, theme::border());
        combo.onChange = [this] { applyInputMapping(); };
        addAndMakeVisible(combo);
    };
    styleInputCombo(modeCombo);
    styleInputCombo(inputComboA);
    styleInputCombo(inputComboB);
    inputChip.setVisible(false);
    modeCombo.setVisible(false);
    inputComboA.setVisible(false);
    inputComboB.setVisible(false);

    const auto styleSectionHeader = [this](juce::Label& label, const juce::String& text) {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, theme::dim());
        addAndMakeVisible(label);
    };
    // Two different concepts live in this column: the channel's own processors
    // and the VST inserts, so they get their own headings.
    styleSectionHeader(fxHeader, "Channel FX");
    styleSectionHeader(insertsHeader, "Inserts");

    eqButton.setColour(juce::TextButton::buttonColourId, theme::control());
    eqButton.setColour(juce::TextButton::textColourOffId, theme::text());
    eqButton.setTooltip("Click: EQ on/off. Right-click: open editor");
    eqButton.onClick = [this] {
        if (auto* ch = currentChannel()) {
            const bool bypassed = ch->eqBypassed.get();
            audioEngine.setChannelEqBypassed(ch->id, !bypassed);
        } else if (auto* out = currentOutput()) {
            const auto& outs = audioEngine.getOutputs();
            for (int i = 0; i < (int)outs.size(); ++i) {
                if (outs[(size_t)i]->id == (std::uint32_t)selectedId) {
                    audioEngine.setOutputEqBypassed(i, !out->eqBypassed.get());
                    break;
                }
            }
        }
        refreshEqButton();
    };
    eqButton.onRightClick = [this] { focusEq(); };
    addAndMakeVisible(eqButton);

    const auto setupDynamicsButton = [this](EqButton& button,
                                            pluginrack::DynamicsEffectType type,
                                            const juce::String& tooltip) {
        button.setColour(juce::TextButton::buttonColourId, theme::control());
        button.setColour(juce::TextButton::textColourOffId, theme::text());
        button.setTooltip(tooltip);
        button.onClick = [this, type] { toggleDynamicsBypass(type); };
        button.onRightClick = [this, type] { focusDynamics(type); };
        addAndMakeVisible(button);
    };
    setupDynamicsButton(compressorButton, pluginrack::DynamicsEffectType::Compressor,
                        "Click: compressor on/off. Right-click: open settings");
    setupDynamicsButton(gateButton, pluginrack::DynamicsEffectType::Gate,
                        "Click: gate on/off. Right-click: open the compressor panel's gate row");
    // The gate lives in the compressor panel now, so both right-clicks land there.
    gateButton.onRightClick = [this] { focusDynamics(pluginrack::DynamicsEffectType::Compressor); };
    refreshDynamicsButtons();

    chainMarker.setColour(juce::Label::backgroundColourId, theme::cyan());
    chainMarker.setInterceptsMouseClicks(false, false);
    slotContent.addAndMakeVisible(chainMarker);

    slotViewport.setViewedComponent(&slotContent, false);
    slotViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(slotViewport);

    noEditorLabel.setJustificationType(juce::Justification::centred);
    noEditorLabel.setFont(theme::ui::bodyFont());
    noEditorLabel.setColour(juce::Label::textColourId, theme::dim());
    editorHolder.addAndMakeVisible(noEditorLabel);

    noEditorBypass.setClickingTogglesState(true);
    noEditorBypass.setColour(juce::TextButton::buttonColourId, theme::control());
    noEditorBypass.setColour(juce::TextButton::buttonOnColourId, theme::amber());
    noEditorBypass.setColour(juce::TextButton::textColourOffId, theme::text());
    noEditorBypass.setTooltip("Bypass the focused plugin");
    noEditorBypass.onClick = [this] { setPluginBypass(focusedInsertId, noEditorBypass.getToggleState()); };
    editorHolder.addAndMakeVisible(noEditorBypass);

    editorViewport.setViewedComponent(&editorHolder, false);
    editorViewport.setScrollBarsShown(true, true);
    addAndMakeVisible(editorViewport);

    outputHeader.setText("Output", juce::dontSendNotification);
    outputHeader.setJustificationType(juce::Justification::centredLeft);
    outputHeader.setFont(theme::ui::itemFont());
    outputHeader.setColour(juce::Label::textColourId, theme::amber());
    addAndMakeVisible(outputHeader);
    addAndMakeVisible(sourceList);

    emptyLabel.setText("No chains yet - add a channel or output in the Mixer view.",
                       juce::dontSendNotification);
    emptyLabel.setJustificationType(juce::Justification::centred);
    emptyLabel.setFont(theme::ui::bodyFont());
    emptyLabel.setColour(juce::Label::textColourId, theme::dim());
    emptyLabel.setVisible(false);
    addAndMakeVisible(emptyLabel);

    addAndMakeVisible(touchBar);

    tapLeft.assign(static_cast<std::size_t>(MiniSpectrum::kFftSize), 0.0f);
    tapRight.assign(static_cast<std::size_t>(MiniSpectrum::kFftSize), 0.0f);

    rebuild();
}

RackView::~RackView() {
    stopTimer();
    releaseMiniTap();
    closeEqWindow();
    closeDynamicsWindow();
    // Stop the capture worker before the editor it captures goes away.
    thumbnailWorker.stop();
    closeDockedEditor();
}

std::uint32_t RackView::selectedChainId() const {
    return selectedId < 0 ? 0 : static_cast<std::uint32_t>(selectedId);
}

void RackView::setSelectedChannel(int channelId, bool isOutput) {
    if (channelId == selectedId && isOutput == selectedIsOutput && channelId >= 0) return;
    closeEqWindow();
    // The dynamics panel is bound to the strip it was opened on; leaving it up
    // would push its edits into whatever strip becomes selected next.
    closeDynamicsWindow();
    selectedId = channelId;
    selectedIsOutput = isOutput;
    focusedInsertId = 0;
    inputEditing = false;
    rebuild();
}

AudioEngine::Channel* RackView::currentChannel() {
    if (selectedIsOutput || selectedId < 0) return nullptr;
    return audioEngine.getChannelById(static_cast<std::uint32_t>(selectedId));
}

AudioEngine::Output* RackView::currentOutput() {
    if (!selectedIsOutput || selectedId < 0) return nullptr;
    return audioEngine.getOutputById(static_cast<std::uint32_t>(selectedId));
}

const std::vector<std::unique_ptr<AudioEngine::InsertSlot>>* RackView::currentInserts() const {
    if (selectedId < 0) return nullptr;
    const auto id = static_cast<std::uint32_t>(selectedId);
    if (!selectedIsOutput) {
        for (const auto& ch : audioEngine.getChannels())
            if (ch->id == id) return &ch->inserts;
    } else {
        for (const auto& out : audioEngine.getOutputs())
            if (out->id == id) return &out->inserts;
    }
    return nullptr;
}

AudioEngine::InsertSlot* RackView::findInsert(std::uint32_t insertId) {
    const auto* inserts = currentInserts();
    if (inserts == nullptr) return nullptr;
    for (const auto& insert : *inserts) {
        if (insert->id == insertId) return insert.get();
    }
    return nullptr;
}

void RackView::ensureSelection() {
    const auto id = selectedId < 0 ? 0 : static_cast<std::uint32_t>(selectedId);
    bool ok = false;
    if (selectedId >= 0) {
        ok = selectedIsOutput ? audioEngine.getOutputById(id) != nullptr
                              : audioEngine.getChannelById(id) != nullptr;
    }
    if (!ok) {
        const auto oldId = selectedId;
        const auto oldOut = selectedIsOutput;
        selectedId = -1;
        if (!audioEngine.getChannels().empty()) {
            selectedId = static_cast<int>(audioEngine.getChannels().front()->id);
            selectedIsOutput = false;
        } else if (!audioEngine.getOutputs().empty()) {
            selectedId = static_cast<int>(audioEngine.getOutputs().front()->id);
            selectedIsOutput = true;
        }
        if (selectedId != oldId || selectedIsOutput != oldOut) {
            closeEqWindow();
            focusedInsertId = 0;
        }
    }
}

void RackView::rebuildChannelCombo() {
    channelCombo.clear();
    comboIds.clear();
    comboIsOutput.clear();
    int item = 1;
    for (const auto& ch : audioEngine.getChannels()) {
        channelCombo.addItem(ch->name, item++);
        comboIds.push_back((int)ch->id);
        comboIsOutput.push_back(0);
    }
    for (const auto& out : audioEngine.getOutputs()) {
        channelCombo.addItem(out->name + " (out)", item++);
        comboIds.push_back((int)out->id);
        comboIsOutput.push_back(1);
    }
    // select current without firing onChange
    for (size_t i = 0; i < comboIds.size(); ++i) {
        if (comboIds[i] == selectedId && (comboIsOutput[i] != 0) == selectedIsOutput) {
            channelCombo.setSelectedItemIndex((int)i, juce::dontSendNotification);
            break;
        }
    }
}

void RackView::refreshHeader() {
    auto* ch = currentChannel();
    auto* out = currentOutput();

    meter.reset();
    if (ch != nullptr) {
        nameLabel.setText(ch->name, juce::dontSendNotification);
        const auto deviceState = audioEngine.getChannelDeviceState(ch->id);
        juce::String sub = deviceState.deviceName.isNotEmpty() ? deviceState.deviceName
                                                               : juce::String("No input");
        sub += ch->layout == pluginrack::ChannelLayout::Mono ? " - Mono" : " - Stereo";
        subLabel.setText(sub, juce::dontSendNotification);
        modeCombo.setSelectedId(ch->layout == pluginrack::ChannelLayout::Mono ? 1 : 2,
                                juce::dontSendNotification);
        refreshInputControls();
        gainSlider.setValue(ch->gainDb.get(), juce::dontSendNotification);
        trimKnob.setValue(ch->trimDb.get(), juce::dontSendNotification);
        panSlider.setValue(ch->pan.get(), juce::dontSendNotification);
        muteButton.setToggleState(ch->muted.get(), juce::dontSendNotification);
        soloButton.setToggleState(ch->soloed.get(), juce::dontSendNotification);
        soloButton.setVisible(true);
        panCaption.setVisible(true);
        panSlider.setVisible(true);
        trimCaption.setVisible(true);
        trimKnob.setVisible(true);
        meter = std::make_unique<LevelMeter>(gainSlider, ch->peakL, ch->peakR, ch->rmsL,
                                             ch->rmsR);
        meter->setStereo(ch->layout != pluginrack::ChannelLayout::Mono);
    } else if (out != nullptr) {
        nameLabel.setText(out->name, juce::dontSendNotification);
        const auto deviceName = audioEngine.getOutputDeviceState(out->id).deviceName;
        subLabel.setText(deviceName.isNotEmpty() ? deviceName : juce::String("No device"),
                         juce::dontSendNotification);
        inputEditing = false;
        inputChip.setVisible(false);
        modeCombo.setVisible(false);
        inputComboA.setVisible(false);
        inputComboB.setVisible(false);
        gainSlider.setValue(out->gainDb.get(), juce::dontSendNotification);
        muteButton.setToggleState(out->muted.get(), juce::dontSendNotification);
        soloButton.setVisible(false);
        panCaption.setVisible(false);
        panSlider.setVisible(false);
        trimCaption.setVisible(false);
        trimKnob.setVisible(false);
        meter = std::make_unique<LevelMeter>(gainSlider, out->peakL, out->peakR, out->rmsL,
                                             out->rmsR);
        meter->setStereo(out->layout != pluginrack::ChannelLayout::Mono);
    }
    if (meter != nullptr) addAndMakeVisible(*meter);
    refreshEqButton();
}

void RackView::rebuildInputCombos(int count) {
    if (count == inputComboCount) return;
    inputComboCount = count;
    inputComboA.clear(juce::dontSendNotification);
    inputComboB.clear(juce::dontSendNotification);
    for (int i = 1; i <= count; ++i) {
        inputComboA.addItem("A" + juce::String(i), i);
        inputComboB.addItem("B" + juce::String(i), i);
    }
}

void RackView::refreshInputControls() {
    auto* ch = currentChannel();
    if (ch == nullptr) {
        inputChip.setVisible(false);
        modeCombo.setVisible(false);
        inputComboA.setVisible(false);
        inputComboB.setVisible(false);
        return;
    }

    const bool mono = ch->layout == pluginrack::ChannelLayout::Mono;
    const auto deviceState = audioEngine.getChannelDeviceState(ch->id);
    const bool hasInputDevice = deviceState.deviceName.isNotEmpty();
    const int a = ch->inputChannels.empty() ? 1 : ch->inputChannels[0] + 1;
    const int b = ch->inputChannels.size() > 1 ? ch->inputChannels[1] + 1 : a;
    modeCombo.setSelectedId(mono ? 1 : 2, juce::dontSendNotification);
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
    inputChip.setVisible(true);
    modeCombo.setVisible(hasInputDevice && inputEditing);
    inputComboA.setVisible(hasInputDevice && inputEditing);
    inputComboB.setVisible(hasInputDevice && inputEditing && !mono);
}

void RackView::applyInputMapping() {
    auto* ch = currentChannel();
    if (ch == nullptr) return;

    const bool mono = modeCombo.getSelectedId() == 1;
    const int a = juce::jmax(0, juce::jmax(1, inputComboA.getSelectedId()) - 1);
    std::vector<int> mapping;
    if (mono) {
        mapping = {a};
    } else {
        int b = juce::jmax(0, juce::jmax(1, inputComboB.getSelectedId()) - 1);
        if (b == a) b = a == 0 ? 1 : 0;
        mapping = {a, b};
    }
    audioEngine.setChannelMapping(ch->id,
                                  mono ? pluginrack::ChannelLayout::Mono
                                  : pluginrack::ChannelLayout::Stereo,
                                  mapping);
    refreshHeader();
    resized();
}

const pluginrack::DynamicsEffect* RackView::dynamicsEffectFor(std::uint32_t itemId) {
    const auto* effects = selectedIsOutput
                              ? (currentOutput() == nullptr ? nullptr : &currentOutput()->effects)
                              : (currentChannel() == nullptr ? nullptr : &currentChannel()->effects);
    if (effects == nullptr) return nullptr;
    const auto byId = std::find_if(effects->begin(), effects->end(),
                                   [itemId](const auto& effect) { return effect.id == itemId; });
    if (byId != effects->end()) return &*byId;
    // The chain row's order names the effect (row 1 compressor, row 2 gate),
    // which is also how the row is labelled. A scene saved while the gate had
    // a different id still resolves through it.
    const auto wanted = itemId == 1 ? pluginrack::DynamicsEffectType::Compressor
                                    : pluginrack::DynamicsEffectType::Gate;
    const auto byType = std::find_if(effects->begin(), effects->end(),
                                     [wanted](const auto& effect) { return effect.type == wanted; });
    return byType != effects->end() ? &*byType : nullptr;
}

const std::vector<pluginrack::ChainItem>* RackView::currentChain() const {
    if (selectedId < 0) return nullptr;
    const auto id = static_cast<std::uint32_t>(selectedId);
    if (!selectedIsOutput) {
        for (const auto& channel : audioEngine.getChannels())
            if (channel->id == id) return &channel->chain;
    } else {
        for (const auto& output : audioEngine.getOutputs())
            if (output->id == id) return &output->chain;
    }
    return nullptr;
}

const std::vector<pluginrack::DynamicsEffect>* RackView::currentEffects() const {
    if (selectedId < 0) return nullptr;
    const auto id = static_cast<std::uint32_t>(selectedId);
    if (!selectedIsOutput) {
        for (const auto& channel : audioEngine.getChannels())
            if (channel->id == id) return &channel->effects;
    } else {
        for (const auto& output : audioEngine.getOutputs())
            if (output->id == id) return &output->effects;
    }
    return nullptr;
}

const pluginrack::DynamicsEffect* RackView::effectOfType(pluginrack::DynamicsEffectType type) const {
    const auto* effects = currentEffects();
    if (effects == nullptr) return nullptr;
    const auto found = std::find_if(effects->begin(), effects->end(),
                                    [type](const auto& effect) { return effect.type == type; });
    return found != effects->end() ? &*found : nullptr;
}

void RackView::refreshSlots() {
    slots.clear();
    eqCard = nullptr;
    dynamicsCard = nullptr;
    dynamicsCardPaired = false;
    vstCards.clear();
    gateButton.setVisible(false);
    eqButton.setVisible(false);
    compressorButton.setVisible(false);
    const auto* inserts = currentInserts();
    const auto* chain = selectedIsOutput
                            ? (currentOutput() == nullptr ? nullptr : &currentOutput()->chain)
                            : (currentChannel() == nullptr ? nullptr : &currentChannel()->chain);
    std::vector<pluginrack::ChainItem> fallback = {
        {pluginrack::ChainItemType::Eq, 0},
        {pluginrack::ChainItemType::Dynamics, 1},
        {pluginrack::ChainItemType::Dynamics, 2}};
    const auto& items = (chain != nullptr && !chain->empty()) ? *chain : fallback;
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;

    int row = 0;
    int y = kSlotInset;
    const int width = juce::jmax(0, slotViewport.getWidth() - 2 * kSlotInset);
    // Rows no longer share a pitch: a card carrying a mini view is taller than
    // a plain one, so each row is placed from the running cursor.
    const auto place = [&](InsertSlotComponent* slot) {
        slotContent.addAndMakeVisible(*slot);
        slot->setBounds(kSlotInset, y, width, slot->preferredHeight());
        slot->setVisible(true);
        y += slot->preferredHeight() + kSlotGap;
        ++row;
    };

    const auto* effectList = currentEffects();
    static const std::vector<pluginrack::DynamicsEffect> fallbackEffects =
        pluginrack::defaultDynamicsEffects();
    const auto& effects = effectList != nullptr ? *effectList : fallbackEffects;
    const auto rows = pluginrack::buildChainRows(items, effects);

    // Exactly one of these is live: currentChannel() refuses an output
    // selection and currentOutput() refuses a channel one.
    auto* selectedChannel = currentChannel();
    auto* selectedOutput = selectedChannel == nullptr ? currentOutput() : nullptr;
    static const std::vector<pluginrack::EqBand> noEqBands;
    const auto& eqBands = selectedChannel != nullptr ? selectedChannel->eqBands
                          : selectedOutput != nullptr ? selectedOutput->eqBands
                                                      : noEqBands;
    const bool eqBypassed = selectedChannel != nullptr ? selectedChannel->eqBypassed.get()
                            : selectedOutput != nullptr ? selectedOutput->eqBypassed.get()
                                                        : false;
    const auto* compressor = effectOfType(pluginrack::DynamicsEffectType::Compressor);

    for (const auto& rowIndices : rows) {
        std::vector<pluginrack::ChainItem> rowItems;
        rowItems.reserve(rowIndices.size());
        for (const int index : rowIndices) rowItems.push_back(items[static_cast<std::size_t>(index)]);
        const auto item = rowItems.front();

        if (item.type == pluginrack::ChainItemType::Insert) {
            AudioEngine::InsertSlot* found = nullptr;
            if (inserts != nullptr)
                for (const auto& candidate : *inserts)
                    if (candidate->id == item.id) {
                        found = candidate.get();
                        break;
                    }
            if (found == nullptr) continue;
            auto* slot = slots.add(new InsertSlotComponent(*this, row));
            slot->setInsert(found);
            auto mini = std::make_unique<InsertMiniView>();
            mini->showVst();
            if (const auto thumb = vstThumbs.find(found->id); thumb != vstThumbs.end())
                mini->setVstSnapshot(thumb->second);
            slot->setMiniView(std::move(mini));
            slot->setSelected(dockedHost != nullptr && focusedInsertId == found->id);
            vstCards[found->id] = slot;
            place(slot);
            continue;
        }

        auto* slot = slots.add(new InsertSlotComponent(*this, row));
        if (rowItems.size() > 1) {
            // Compressor and gate share the Dynamics card: one switch each, and
            // the panel a click opens is the one that carries both.
            const int compIndex =
                pluginrack::compressorIndexInRow(items, effects, rowIndices);
            const int gateIndex =
                rowIndices.front() == compIndex ? rowIndices.back() : rowIndices.front();
            const auto compressorItem = items[static_cast<std::size_t>(compIndex)];
            const auto gateItem = items[static_cast<std::size_t>(gateIndex)];
            const auto* gate = effectOfType(pluginrack::DynamicsEffectType::Gate);
            CardSwitch compSwitch{compressorItem, compressor != nullptr && compressor->bypassed, "C",
                                  "Compressor: click to bypass, right-click to open the panel"};
            CardSwitch gateSwitch{gateItem, gate != nullptr && gate->bypassed, "G",
                                  "Gate: click to bypass, right-click to open the panel"};
            slot->setBuiltInPair("Dynamics", compressorItem, compSwitch, gateSwitch);
            auto mini = std::make_unique<InsertMiniView>();
            if (compressor != nullptr && gate != nullptr)
                mini->showDynamicsPair(*compressor, *gate);
            else if (const auto* single = compressor != nullptr ? compressor : gate)
                mini->showDynamics(*single);
            slot->setMiniView(std::move(mini));
            slot->setSelected(dynamicsFocused);
            dynamicsCard = slot;
            dynamicsCardPaired = true;
            place(slot);
            continue;
        }

        if (item.type == pluginrack::ChainItemType::Eq) {
            slot->setBuiltIn(item, "EQ", eqBypassed);
            auto mini = std::make_unique<InsertMiniView>();
            mini->showEq(eqBands, liveRateHz);
            slot->setMiniView(std::move(mini));
            slot->setSelected(eqFocused);
            eqCard = slot;
        } else {
            // A chain missing its partner leaves one dynamics effect on a row
            // of its own.
            const auto* effect = dynamicsEffectFor(item.id);
            const bool isCompressor =
                effect != nullptr ? effect->type == pluginrack::DynamicsEffectType::Compressor
                                  : item.id == 1;
            slot->setBuiltIn(item, isCompressor ? "Compressor" : "Gate",
                             effect != nullptr && effect->bypassed);
            if (effect != nullptr) {
                auto mini = std::make_unique<InsertMiniView>();
                mini->showDynamics(*effect);
                slot->setMiniView(std::move(mini));
                dynamicsCard = slot;
                dynamicsCardPaired = false;
                slot->setSelected(dynamicsFocused);
            }
        }
        place(slot);
    }
    chainRowCount = row;

    // Pad the list out to the fixed 8-slot chain; the spare VST rows are the
    // add control, so the insert list never changes length while you work. A
    // chain already holding its five VSTs gets no add row: the engine refuses
    // a sixth insert, and the merged Dynamics card leaves one row spare here,
    // so the row would only open a picker that then has to refuse the plugin.
    const bool canAddVst =
        inserts == nullptr || static_cast<int>(inserts->size()) < kChainVstSlots;
    const int totalRows = canAddVst ? juce::jmax(kChainSlotCount, chainRowCount) : chainRowCount;
    for (; row < totalRows; ++row) {
        auto* slot = slots.add(new InsertSlotComponent(*this, row));
        slot->setAddSlot(row == chainRowCount);
        place(slot);
    }

    // A thumbnail outlives the card it was captured for, but not the insert.
    for (auto it = vstThumbs.begin(); it != vstThumbs.end();)
        it = vstCards.count(it->first) != 0 ? std::next(it) : vstThumbs.erase(it);

    slotContent.setSize(juce::jmax(0, slotViewport.getWidth()), juce::jmax(0, y));
    if (chainDropTarget >= 0) {
        chainMarker.setBounds(kSlotInset, chainBoundaryY(chainDropTarget), width, 2);
        chainMarker.setVisible(true);
        chainMarker.toFront(false);
    } else {
        chainMarker.setVisible(false);
    }
    startMiniTimer();
}

void RackView::visibilityChanged() {
    if (isShowing()) startMiniTimer();
    else {
        stopTimer();
        releaseMiniTap();
    }
}

AudioEngine::AnalyzerTap* RackView::currentTap() {
    if (auto* channel = currentChannel()) return &channel->analyzerTap;
    if (auto* output = currentOutput()) return &output->analyzerTap;
    return nullptr;
}

void RackView::releaseMiniTap() {
    // Resolved by id, never by pointer: a scene restore destroys the strip the
    // tap lived on, so a stored pointer could outlive it.
    if (tapArmedId == 0) return;
    if (tapArmedIsOutput) {
        for (const auto& output : audioEngine.getOutputs())
            if (output->id == tapArmedId) output->analyzerTap.setMode(AudioEngine::AnalyzerTap::kOff);
    } else {
        for (const auto& channel : audioEngine.getChannels())
            if (channel->id == tapArmedId) channel->analyzerTap.setMode(AudioEngine::AnalyzerTap::kOff);
    }
    tapArmedId = 0;
}

void RackView::startMiniTimer() {
    const bool hasMinis = eqCard != nullptr || dynamicsCard != nullptr || !vstCards.empty();
    if (!hasMinis) {
        stopTimer();
        releaseMiniTap();
        return;
    }
    // Deliberately not gated on isShowing(): visibilityChanged() can fire
    // before the window is on screen, and the callback already no-ops while
    // the view is hidden. Restarting only when the rate changes keeps a layout
    // pass from resetting the interval.
    const int fps = juce::jlimit(1, 60, audioEngine.getRackThumbnailFps());
    if (isTimerRunning() && fps == miniTimerHz) return;
    miniTimerHz = fps;
    startTimerHz(fps);
}

void RackView::timerCallback() {
    if (!isShowing()) {
        releaseMiniTap();
        return;
    }
    // JUCE re-syncs a plugin's native window whenever the plugin resizes itself;
    // keep it parked inside the bay.
    if (dockedEditor != nullptr) layoutDockedEditor();
    const int fps = juce::jlimit(1, 60, audioEngine.getRackThumbnailFps());
    if (fps != miniTimerHz) {
        miniTimerHz = fps;
        startTimerHz(fps);
    }

    // An output's row list follows live routing and device state; both only
    // move on user action, so a few updates a second is plenty.
    if (selectedIsOutput && sourceList.isVisible()) {
        const auto now = juce::Time::getMillisecondCounter();
        if (lastSourceRefreshMs == 0 || now - lastSourceRefreshMs >= 250) {
            lastSourceRefreshMs = now;
            refreshSourceList();
        }
    } else {
        lastSourceRefreshMs = 0;
    }

    // Built-in thumbnails read the strip's analyzer tap. It is re-armed every
    // tick because closing another view's panel switches the same tap off.
    auto* tap = (eqCard != nullptr || dynamicsCard != nullptr)
                    ? currentTap()
                    : nullptr;
    if (tap != nullptr) {
        // Selecting another strip would otherwise leave the previous one
        // copying every block into its rings for the rest of the session.
        if (tapArmedId != 0
            && (tapArmedId != selectedChainId() || tapArmedIsOutput != selectedIsOutput)) {
            releaseMiniTap();
        }
        tap->setMode(AudioEngine::AnalyzerTap::kPost);
        tapArmedId = selectedChainId();
        tapArmedIsOutput = selectedIsOutput;
    }

    // The cards follow the engine, not just the layout: the EQ panel and the
    // dynamics knobs edit these values while the list stays on screen.
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
    const bool eqBypassed = selectedIsOutput
                                ? (currentOutput() != nullptr && currentOutput()->eqBypassed.get())
                                : (currentChannel() != nullptr && currentChannel()->eqBypassed.get());
    if (eqCard != nullptr) {
        if (auto* channel = currentChannel()) {
            if (auto* mini = eqCard->miniView()) {
                mini->setEqBands(channel->eqBands, liveRateHz);
                mini->setSpectrumOffsetDb(channel->eqRtaOffsetDb);
                mini->setWeighting(channel->eqRtaWeighting);
            }
        } else if (auto* output = currentOutput()) {
            if (auto* mini = eqCard->miniView()) {
                mini->setEqBands(output->eqBands, liveRateHz);
                mini->setSpectrumOffsetDb(output->eqRtaOffsetDb);
                mini->setWeighting(output->eqRtaWeighting);
            }
        }
        if (eqCard != nullptr) eqCard->setBuiltInBypassed(eqBypassed);
    }

    if (tap != nullptr && eqCard != nullptr) {
        tap->snapshot(true, tapLeft.data(), tapRight.data(), MiniSpectrum::kFftSize);
        miniSpectrum.refresh(tapLeft.data(), tapRight.data());
        if (auto* mini = eqCard->miniView())
            mini->setSpectrum(miniSpectrum.magnitudes(), miniSpectrum.numBins());
    }

    if (tap != nullptr && dynamicsCard != nullptr) {
        tap->snapshot(false, tapLeft.data(), tapRight.data(), kLevelFrames);
        const auto* compressorEffect = effectOfType(pluginrack::DynamicsEffectType::Compressor);
        const auto* gateEffect = effectOfType(pluginrack::DynamicsEffectType::Gate);

        // The card is one block, so it reports one combined reduction, the same
        // sum the editor's gauge shows.
        float levelDb = -60.0f;
        float combinedGainDb = 0.0f;
        if (compressorEffect != nullptr) {
            compressorMeter.update(tapLeft.data(), tapRight.data(), kLevelFrames,
                                   *compressorEffect, static_cast<double>(fps));
            levelDb = compressorMeter.inputDb;
            combinedGainDb += compressorMeter.gainDb;
        }
        if (gateEffect != nullptr) {
            gateMeter.update(tapLeft.data(), tapRight.data(), kLevelFrames, *gateEffect,
                             static_cast<double>(fps));
            if (compressorEffect == nullptr) levelDb = gateMeter.inputDb;
            combinedGainDb += gateMeter.gainDb;
        }

        if (dynamicsCardPaired) {
            dynamicsCard->setPairBypassed(compressorEffect != nullptr && compressorEffect->bypassed,
                                          gateEffect != nullptr && gateEffect->bypassed);
        } else if (const auto* single = compressorEffect != nullptr ? compressorEffect : gateEffect) {
            dynamicsCard->setBuiltInBypassed(single->bypassed);
        }

        if (auto* mini = dynamicsCard->miniView()) {
            if (compressorEffect != nullptr && gateEffect != nullptr)
                mini->setDynamicsPair(*compressorEffect, *gateEffect);
            else if (compressorEffect != nullptr)
                mini->setDynamics(*compressorEffect);
            else if (gateEffect != nullptr)
                mini->setDynamics(*gateEffect);
            if (compressorEffect != nullptr || gateEffect != nullptr)
                mini->setLiveGain(levelDb, combinedGainDb);
        }
    }

    // The focused VST's card shows what its editor looks like. JUCE hands out
    // one editor per processor, so a live thumbnail and the bay cannot
    // coexist; and a hosted VST3 editor paints itself into its own window, so
    // it is captured from that window when it has one.
    //
    // That capture must not run here: PrintWindow makes the plugin repaint its
    // whole UI, and doing that from a message-thread timer at display rates
    // starved every other message (the window froze while audio kept running).
    const bool wantsThumbnail = dockedEditor != nullptr && focusedInsertId != 0
                                && !eqFocused && !dynamicsFocused
                                && dockedEditor->isShowing();
    if (!wantsThumbnail) {
        thumbnailWorker.clearRequest();
        return;
    }

    const int targetWidth = juce::jmax(32, slotViewport.getWidth() - 2 * kSlotInset);
    if (auto* pluginWindow = nativedisplay::pluginWindowOf(*dockedEditor)) {
        // The worker keeps only the newest request, so a slow plugin lowers
        // the thumbnail rate instead of backing the UI thread up.
        const auto insertId = focusedInsertId;
        juce::Component::SafePointer<RackView> safe(this);
        thumbnailWorker.request(pluginWindow, targetWidth,
                                [safe, insertId](juce::Image thumbnail) {
            if (safe != nullptr) safe->applyVstThumbnail(insertId, thumbnail);
        });
        return;
    }

    // A software-drawn editor can only be rendered on the message thread, so
    // pace it with what the previous snapshot cost.
    const auto now = juce::Time::getMillisecondCounter();
    const auto interval = static_cast<juce::uint32>(
        juce::jmax(1000 / juce::jmax(1, fps), 4 * lastSnapshotCostMs));
    if (now - lastSnapshotMs < interval) return;
    lastSnapshotMs = now;

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto editorBounds = dockedEditor->getLocalBounds();
    juce::Image thumb;
    if (!editorBounds.isEmpty()) {
        const float scale = juce::jlimit(0.02f, 1.0f,
                                         static_cast<float>(targetWidth)
                                             / static_cast<float>(editorBounds.getWidth()));
        thumb = dockedEditor->createComponentSnapshot(editorBounds, true, scale);
    }
    lastSnapshotCostMs =
        juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - started);
    applyVstThumbnail(focusedInsertId, thumb);
}

void RackView::applyVstThumbnail(std::uint32_t insertId, const juce::Image& thumbnail) {
    if (thumbnail.isNull()) return;
    vstThumbs[insertId] = thumbnail;
    if (const auto card = vstCards.find(insertId); card != vstCards.end())
        if (auto* mini = card->second->miniView()) mini->setVstSnapshot(thumbnail);
}

void RackView::refreshEditor() {
    if (eqFocused && eqWindow != nullptr) {
        hideDockedEditor();
        if (eqWindow->getParentComponent() != &editorHolder)
            editorHolder.addAndMakeVisible(eqWindow);
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
        resized();
        return;
    }
    // The dynamics panel owns the bay while it is open; docking an insert
    // editor here would paint it on top of the panel.
    if (dynamicsFocused && dynamicsWindow != nullptr) {
        hideDockedEditor();
        if (dynamicsWindow->getParentComponent() != &editorHolder)
            editorHolder.addAndMakeVisible(dynamicsWindow);
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
        resized();
        return;
    }
    if (eqFocused) {
        eqFocused = false;
    }
    if (eqWindow != nullptr) {
        auto* doomed = eqWindow;
        eqWindow = nullptr;
        editorHolder.removeChildComponent(doomed);
        delete doomed;
    }

    auto* focus = focusedInsertId != 0 ? findInsert(focusedInsertId) : nullptr;
    if (focus == nullptr) {
        if (const auto* inserts = currentInserts(); inserts != nullptr && !inserts->empty()) {
            focus = inserts->front().get();
            focusedInsertId = focus->id;
        }
    }
    if (focus == nullptr) {
        closeDockedEditor();
        noEditorLabel.setText("No plugins - click + Add VST3 above", juce::dontSendNotification);
        noEditorLabel.setVisible(true);
        noEditorBypass.setVisible(false);
        dockedEditor = nullptr;
        dockedHost = nullptr;
        resized();
        return;
    }

    // Knobs turned inside the plugin's own window become the touch strip's
    // target (the plugin reports the parameter change; the click itself never
    // reaches JUCE). Re-armed below after any closeDockedEditor() call.
    auto* pluginInstance = focus->pluginHost.getInstance();

    // Already docked for this insert: reuse the live editor (it may only be
    // hidden behind an EQ/dynamics panel) instead of rebuilding the VST UI.
    if (dockedEditor != nullptr && dockedHost == &focus->pluginHost) {
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
        if (dockedEditor->getParentComponent() != &editorHolder)
            editorHolder.addAndMakeVisible(dockedEditor);
        dockedEditor->setVisible(true);
        knobWatcher.setPlugin(pluginInstance);
        resized();
        return;
    }
    closeDockedEditor();

    dockedHost = &focus->pluginHost;
    dockedHost->closeFloatingEditor();
    dockedEditor = dockedHost->createEditorForDock();
    if (dockedEditor == nullptr) {
        dockedHost = nullptr;
        // No GUI: there is nothing to dock or touch (the card still shows the
        // plugin's name and its bypass switch).
        noEditorLabel.setText("This plugin has no editor", juce::dontSendNotification);
        noEditorLabel.setVisible(true);
        noEditorBypass.setVisible(true);
        noEditorBypass.setToggleState(focus->bypassed.get(), juce::dontSendNotification);
        knobWatcher.setPlugin(pluginInstance);
        resized();
        return;
    }

    // The plugin's own size at 100%: the fit is measured from this, so no
    // layout pass ever feeds back into the size it scales from.
    dockedEditorNaturalSize = dockedEditor->getBounds();
    dockedEditorScale = 1.0f;

    noEditorLabel.setVisible(false);
    noEditorBypass.setVisible(false);
    const auto dockStart = juce::Time::getMillisecondCounterHiRes();
    auto dockElapsed = [dockStart] {
        return juce::String(juce::Time::getMillisecondCounterHiRes() - dockStart, 1);
    };
    dockedEditorLayoutPasses = 0;
    dockedEditor->addComponentListener(this);
    editorHolder.addAndMakeVisible(dockedEditor);
    knobWatcher.setPlugin(pluginInstance);
    resized();
    // Some plugins (Native Instruments' Qt editors) create their own native
    // window at their preferred size and put it back after it has been placed.
    // Re-apply the bay fit once the plugin has settled instead of fighting it
    // in a resize loop.
    juce::Component::SafePointer<RackView> safe(this);
    for (const int delayMs : {400, 1200}) {
        juce::Timer::callAfterDelay(delayMs, [safe] {
            if (safe == nullptr) return;
            safe->dockedEditorLayoutPasses = 0;
            safe->layoutDockedEditor();
        });
    }
    // Docking a native plugin UI is the slow part of adding one (NI editors take
    // ~1-2.5 s to attach), so it is reported in the run log.
    juce::Logger::writeToLog("rack: editor docked " + dockElapsed() + " ms");
}

void RackView::layoutDockedEditor() {
    if (dockedEditor == nullptr || layingOutDockedEditor) return;
    const juce::ScopedValueSetter<bool> guard(layingOutDockedEditor, true);
    const auto bay = editorHolder.getLocalBounds();
    if (bay.isEmpty()) return;

    applyDockedEditorFit(bay);

    // What the editor paints now: a VST3 re-sizes itself when its content scale
    // changes, while a software-drawn editor keeps its own size and is scaled
    // by its transform instead. Either way the result is centred in the bay and
    // never leaves it.
    const auto visual = dockedEditorVisualBounds();
    const auto fitted = bay.withSizeKeepingCentre(
        juce::jmax(1, juce::jmin(visual.getWidth(), bay.getWidth())),
        juce::jmax(1, juce::jmin(visual.getHeight(), bay.getHeight())));

    if (!dockedEditor->getTransform().isIdentity()) {
        // The transform scales away from the component's top-left, so the
        // position is what puts the scaled drawing in the middle.
        if (dockedEditor->getPosition() != fitted.getPosition())
            dockedEditor->setTopLeftPosition(fitted.getPosition());
    } else if (dockedEditor->getBounds() != fitted) {
        dockedEditor->setBounds(fitted);
        juce::Logger::writeToLog("rack: editor placed " + fitted.toString() + " in bay "
                                 + bay.toString());
    }
}

void RackView::applyDockedEditorFit(const juce::Rectangle<int>& bay) {
    if (dockedEditor == nullptr || bay.isEmpty()) return;
    const int naturalWidth = dockedEditorNaturalSize.getWidth();
    const int naturalHeight = dockedEditorNaturalSize.getHeight();
    if (naturalWidth <= 0 || naturalHeight <= 0) return;

    // Whichever of width/height is the tighter constraint decides the scale, so
    // one axis fills the bay and the plugin keeps its own aspect ratio.
    const auto fit = juce::jmin(static_cast<float>(bay.getWidth())
                                    / static_cast<float>(naturalWidth),
                                static_cast<float>(bay.getHeight())
                                    / static_cast<float>(naturalHeight));
    // Ignore sub-percent moves: dragging the window would otherwise re-lay out
    // the plugin UI on every pixel.
    if (fit <= 0.0f || std::abs(fit - dockedEditorScale) < 0.01f) return;

    const auto before = dockedEditorVisualBounds();
    {
        // The plugin re-sizes itself inside this call, and that resize is ours:
        // it must not be read as the plugin choosing a size of its own.
        const juce::ScopedValueSetter<bool> echoGuard(applyingDockedEditorScale, true);
        dockedEditor->setScaleFactor(fit);
    }
    // A plugin that does not support content scaling comes back at the size it
    // already had. Remember that no scale is in effect, so a size the plugin
    // picks later is still read as its own 100% size.
    const auto after = dockedEditorVisualBounds();
    const bool rescaled = after.getWidth() != before.getWidth()
                       || after.getHeight() != before.getHeight();
    dockedEditorScale = rescaled ? fit : 1.0f;
}

juce::Rectangle<int> RackView::dockedEditorVisualBounds() const {
    if (dockedEditor == nullptr) return {};
    // Only a pure scale is ever applied (AudioProcessorEditor::setScaleFactor
    // installs exactly AffineTransform::scale), so the matrix diagonals are it.
    const auto& transform = dockedEditor->getTransform();
    return { dockedEditor->getX(), dockedEditor->getY(),
             juce::roundToInt(static_cast<float>(dockedEditor->getWidth()) * transform.mat00),
             juce::roundToInt(static_cast<float>(dockedEditor->getHeight()) * transform.mat11) };
}

void RackView::componentMovedOrResized(juce::Component& component, bool, bool wasResized) {
    if (!wasResized || &component != dockedEditor || layingOutDockedEditor) return;

    if (!applyingDockedEditorScale && dockedEditorScale > 0.0f) {
        // A resize that is not ours is the plugin picking a new UI size (some
        // grow when a panel inside them is expanded). Move the baseline the fit
        // is measured from, or the next pass would shrink it straight back.
        const auto visual = dockedEditorVisualBounds();
        const auto natural = juce::Point<int>(
            juce::roundToInt(static_cast<float>(visual.getWidth()) / dockedEditorScale),
            juce::roundToInt(static_cast<float>(visual.getHeight()) / dockedEditorScale));
        const auto drifted = std::abs(natural.x - dockedEditorNaturalSize.getWidth())
                                 > juce::jmax(2, dockedEditorNaturalSize.getWidth() / 50)
                             || std::abs(natural.y - dockedEditorNaturalSize.getHeight())
                                 > juce::jmax(2, dockedEditorNaturalSize.getHeight() / 50);
        if (!visual.isEmpty() && drifted)
            dockedEditorNaturalSize = { 0, 0, natural.x, natural.y };
    }

    // A plugin is free to size itself once it is on screen; put it back, fitted
    // and centred in the bay. Two passes settle it without fighting a plugin
    // that keeps insisting on a size the bay cannot hold.
    if (dockedEditorLayoutPasses >= 2) return;
    ++dockedEditorLayoutPasses;
    juce::Component::SafePointer<RackView> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        safe->layoutDockedEditor();
    });
}

void RackView::releaseOrphanedDockedEditor() {
    if (dockedHost != nullptr && dockedHostAlive()) return;
    closeDockedEditor();
}

void RackView::closeDockedEditor() {
    // The knob watcher reads the processor directly: drop it before the insert
    // (and with it the processor) can be destroyed.
    knobWatcher.clear();
    thumbnailWorker.clearRequest();
    if (dockedEditor == nullptr) {
        dockedHost = nullptr;
        return;
    }
    // A capture may be reading the editor's native window right now; hold the
    // worker's lock so it has finished before that window is destroyed.
    const juce::ScopedLock captureGuard(thumbnailWorker.captureLock());
    dockedEditor->removeComponentListener(this);
    dockedEditorLayoutPasses = 0;
    // A floating window may have taken ownership of the editor (reparented
    // away from the dock): the window owns it now, so release our pointers
    // without deleting.
    if (dockedEditor->getParentComponent() != nullptr
        && dockedEditor->getParentComponent() != &editorHolder) {
        dockedEditor = nullptr;
        dockedHost = nullptr;
        dockedEditorNaturalSize = {};
        dockedEditorScale = 1.0f;
        applyingDockedEditorScale = false;
        return;
    }
    editorHolder.removeChildComponent(dockedEditor);
    if (dockedHost != nullptr && dockedHostAlive()) {
        dockedHost->destroyDockEditor(dockedEditor);
    } else {
        // Host is gone: its processor may be gone too, so never touch
        // dockedHost here (it may dangle). Defer the delete past the
        // current mouse handler; our pointers are cleared below.
        juce::Component::SafePointer<juce::Component> safe(dockedEditor);
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr) delete safe.getComponent();
        });
    }
    dockedEditor = nullptr;
    dockedHost = nullptr;
    dockedEditorNaturalSize = {};
    dockedEditorScale = 1.0f;
    applyingDockedEditorScale = false;
}

void RackView::hideDockedEditor() {
    // Nothing to thumbnail while the bay shows a panel instead.
    thumbnailWorker.clearRequest();
    if (dockedEditor != nullptr) dockedEditor->setVisible(false);
}

// Brings the insert editor back once no panel owns the bay any more.
void RackView::showDockedEditor() {
    if (dockedEditor == nullptr || eqFocused || dynamicsFocused) return;
    if (dockedEditor->getParentComponent() != &editorHolder) return;
    dockedEditor->setVisible(true);
    resized();
}

bool RackView::dockedHostAlive() const {
    for (const auto& ch : audioEngine.getChannels()) {
        for (const auto& insert : ch->inserts) {
            if (&insert->pluginHost == dockedHost) return true;
        }
    }
    for (const auto& out : audioEngine.getOutputs()) {
        for (const auto& insert : out->inserts) {
            if (&insert->pluginHost == dockedHost) return true;
        }
    }
    return false;
}

void RackView::focusEq() {
    closeDynamicsWindow();
    if (eqWindow != nullptr) {
        eqFocused = true;
        hideDockedEditor();
        if (eqWindow->getParentComponent() != &editorHolder)
            editorHolder.addAndMakeVisible(eqWindow);
        refreshEqButton();
        resized();
        return;
    }
    if (auto* ch = currentChannel()) {
        juce::Component::SafePointer<RackView> safe(this);
        const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
        auto* window = new EQWindow(
            "EQ - " + ch->name, ch->eqBands,
            [safe](const std::vector<pluginrack::EqBand>& bands) {
                if (safe == nullptr) return;
                safe->audioEngine.setChannelEqBypassed(safe->selectedChainId(), false);
                safe->audioEngine.setChannelEq(safe->selectedChainId(), bands);
                safe->refreshEqButton();
            },
            liveRateHz, true, ch->eqRtaOffsetDb, ch->eqRtaWeighting);
        window->applySpectrumOffset = [safe](float offsetDb) {
            if (safe == nullptr) return;
            safe->audioEngine.setChannelEqRtaOffset(safe->selectedChainId(), offsetDb);
        };
        window->applyWeighting = [safe](int weighting) {
            if (safe == nullptr) return;
            safe->audioEngine.setChannelEqRtaWeighting(safe->selectedChainId(), weighting);
        };
        window->pullAnalyzerTap = [safe](bool postEq, float* destL, float* destR) {
            if (safe == nullptr) return;
            // resolve channel
            if (auto* c = safe->currentChannel())
                c->analyzerTap.snapshot(postEq, destL, destR, EQWindow::kAnalyzerFftSize);
        };
        window->setAnalyzerTapMode = [safe](int mode) {
            if (safe == nullptr) return;
            if (auto* c = safe->currentChannel())
                c->analyzerTap.setMode(static_cast<std::uint32_t>(mode));
        };
        window->setAnalyzerMode(EQWindow::kAnalyzerPost);
        // Delay knob: seed from the channel and persist through the engine,
        // engaging the EQ on edit (same rule as a band drag).
        window->setDelayMs(ch->eqDelayMs);
        window->applyDelayMs = [safe](float delayMs) {
            if (safe == nullptr) return;
            const auto channelId = safe->selectedChainId();
            safe->audioEngine.setChannelEqBypassed(channelId, false);
            safe->audioEngine.setChannelEqDelay(channelId, delayMs);
            safe->refreshEqButton();
        };
        eqWindow = window;
        eqFocused = true;
        hideDockedEditor();
        editorHolder.addAndMakeVisible(eqWindow);
        window->onCloseRequest = [safe] {
            if (safe == nullptr) return;
            safe->closeEqWindow();
        };
        refreshEqButton();
        resized();
    } else if (auto* out = currentOutput()) {
        const std::uint32_t outputId = (std::uint32_t)selectedId;
        juce::Component::SafePointer<RackView> safe(this);
        const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
        auto* window = new EQWindow(
            "EQ - " + out->name, out->eqBands,
            [safe, outputId](const std::vector<pluginrack::EqBand>& bands) {
                if (safe == nullptr) return;
                const auto& outs = safe->audioEngine.getOutputs();
                for (int i = 0; i < (int)outs.size(); ++i) {
                    if (outs[(size_t)i]->id == outputId) {
                        safe->audioEngine.setOutputEqBypassed(i, false);
                        safe->audioEngine.setOutputEq(i, bands);
                        break;
                    }
                }
                safe->refreshEqButton();
            },
            liveRateHz, true, out->eqRtaOffsetDb, out->eqRtaWeighting);
        window->applySpectrumOffset = [safe, outputId](float offsetDb) {
            if (safe == nullptr) return;
            const auto& outs = safe->audioEngine.getOutputs();
            for (int i = 0; i < (int)outs.size(); ++i) {
                if (outs[(size_t)i]->id == outputId) {
                    safe->audioEngine.setOutputEqRtaOffset(i, offsetDb);
                    break;
                }
            }
        };
        window->applyWeighting = [safe, outputId](int weighting) {
            if (safe == nullptr) return;
            const auto& outs = safe->audioEngine.getOutputs();
            for (int i = 0; i < (int)outs.size(); ++i) {
                if (outs[(size_t)i]->id == outputId) {
                    safe->audioEngine.setOutputEqRtaWeighting(i, weighting);
                    break;
                }
            }
        };
        window->pullAnalyzerTap = [safe](bool postEq, float* destL, float* destR) {
            if (safe == nullptr) return;
            if (auto* o = safe->currentOutput())
                o->analyzerTap.snapshot(postEq, destL, destR, EQWindow::kAnalyzerFftSize);
        };
        window->setAnalyzerTapMode = [safe](int mode) {
            if (safe == nullptr) return;
            if (auto* o = safe->currentOutput())
                o->analyzerTap.setMode(static_cast<std::uint32_t>(mode));
        };
        window->setAnalyzerMode(EQWindow::kAnalyzerPost);
        // Delay knob, same contract as the channel branch above.
        window->setDelayMs(out->eqDelayMs);
        window->applyDelayMs = [safe, outputId](float delayMs) {
            if (safe == nullptr) return;
            const auto& outs = safe->audioEngine.getOutputs();
            for (int i = 0; i < (int)outs.size(); ++i) {
                if (outs[(size_t)i]->id == outputId) {
                    safe->audioEngine.setOutputEqBypassed(i, false);
                    safe->audioEngine.setOutputEqDelay(i, delayMs);
                    break;
                }
            }
            safe->refreshEqButton();
        };
        eqWindow = window;
        eqFocused = true;
        hideDockedEditor();
        editorHolder.addAndMakeVisible(eqWindow);
        window->onCloseRequest = [safe] {
            if (safe == nullptr) return;
            safe->closeEqWindow();
        };
        refreshEqButton();
        resized();
    }
}

void RackView::closeEqWindow() {
    if (eqWindow == nullptr) return;
    auto* doomed = eqWindow;
    eqWindow = nullptr;
    eqFocused = false;
    editorHolder.removeChildComponent(doomed);
    delete doomed;
    refreshEqButton();
    showDockedEditor();
}

void RackView::refreshEqButton() {
    bool active = false;
    if (auto* ch = currentChannel()) {
        if (!ch->eqBypassed.get()) {
            // A delay-only EQ still does audible work, so it lights the button.
            active = ch->eqDelayMs > 0.0f;
            for (const auto& band : ch->eqBands) {
                if (band.enabled) { active = true; break; }
            }
        }
    } else if (auto* out = currentOutput()) {
        if (!out->eqBypassed.get()) {
            active = out->eqDelayMs > 0.0f;
            for (const auto& band : out->eqBands) {
                if (band.enabled) { active = true; break; }
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

void RackView::focusDynamics(pluginrack::DynamicsEffectType type) {
    if (dynamicsWindow != nullptr) {
        if (dynamicsWindow->getType() != type) {
            closeDynamicsWindow();
        } else {
            dynamicsFocused = true;
            closeEqWindow();
            if (dynamicsWindow->getParentComponent() != &editorHolder)
                editorHolder.addAndMakeVisible(dynamicsWindow);
            refreshDynamicsButtons();
            resized();
            return;
        }
    }

    const auto effectName = [type] {
        switch (type) {
            case pluginrack::DynamicsEffectType::Compressor: return juce::String("Compressor");
            case pluginrack::DynamicsEffectType::Gate: return juce::String("Gate");
        }
        return juce::String("Effect");
    };
    const auto findEffect = [type](std::vector<pluginrack::DynamicsEffect>& effects) {
        return std::find_if(effects.begin(), effects.end(),
                            [type](const auto& effect) { return effect.type == type; });
    };
    // The panel edits its own effect and the strip's gate, so apply matches on
    // the id it was handed rather than on the opened type.
    const auto findEffectById = [](std::vector<pluginrack::DynamicsEffect>& effects,
                                   std::uint32_t id) {
        return std::find_if(effects.begin(), effects.end(),
                            [id](const auto& effect) { return effect.id == id; });
    };
    const auto findGate = [](std::vector<pluginrack::DynamicsEffect>& effects) {
        return std::find_if(effects.begin(), effects.end(), [](const auto& effect) {
            return effect.type == pluginrack::DynamicsEffectType::Gate;
        });
    };

    juce::Component::SafePointer<RackView> safe(this);
    if (auto* ch = currentChannel()) {
        auto it = findEffect(ch->effects);
        if (it == ch->effects.end()) return;
        auto gateIt = findGate(ch->effects);
        auto* window = new DynamicsWindow(
            effectName() + " - " + ch->name, *it,
            [safe, findEffectById](const pluginrack::DynamicsEffect& effect, bool bypassChanged) {
                if (safe == nullptr) return;
                auto* channel = safe->currentChannel();
                if (channel == nullptr) return;
                auto effects = channel->effects;
                const auto found = findEffectById(effects, effect.id);
                if (found != effects.end()) {
                    const bool bypassed = found->bypassed;
                    *found = effect;
                    // Knob edits keep whatever the strip's bypass says; only
                    // the panel's own switch may change it.
                    if (!bypassChanged) found->bypassed = bypassed;
                    safe->audioEngine.setChannelEffects(channel->id, effects);
                    safe->refreshDynamicsButtons();
                }
            },
            gateIt != ch->effects.end() ? &*gateIt : nullptr);
        window->pullAnalyzerTap = [safe](float* left, float* right, int frames) {
            if (safe == nullptr) return;
            if (auto* channel = safe->currentChannel()) {
                // Re-arm every tick: the tap is shared, so closing an EQ or a
                // panel in another view can switch it off under us.
                channel->analyzerTap.setMode(AudioEngine::AnalyzerTap::kPre);
                channel->analyzerTap.snapshot(false, left, right, frames);
            }
        };
        ch->analyzerTap.setMode(AudioEngine::AnalyzerTap::kPre);
        dynamicsWindow = window;
    } else if (auto* out = currentOutput()) {
        auto it = findEffect(out->effects);
        if (it == out->effects.end()) return;
        auto gateIt = findGate(out->effects);
        auto* window = new DynamicsWindow(
            effectName() + " - " + out->name, *it,
            [safe, findEffectById](const pluginrack::DynamicsEffect& effect, bool bypassChanged) {
                if (safe == nullptr) return;
                auto* output = safe->currentOutput();
                if (output == nullptr) return;
                auto effects = output->effects;
                const auto found = findEffectById(effects, effect.id);
                if (found != effects.end()) {
                    const bool bypassed = found->bypassed;
                    *found = effect;
                    if (!bypassChanged) found->bypassed = bypassed;
                    safe->audioEngine.setOutputEffects(output->id, effects);
                    safe->refreshDynamicsButtons();
                }
            },
            gateIt != out->effects.end() ? &*gateIt : nullptr);
        window->pullAnalyzerTap = [safe](float* left, float* right, int frames) {
            if (safe == nullptr) return;
            if (auto* output = safe->currentOutput()) {
                output->analyzerTap.setMode(AudioEngine::AnalyzerTap::kPre);
                output->analyzerTap.snapshot(false, left, right, frames);
            }
        };
        out->analyzerTap.setMode(AudioEngine::AnalyzerTap::kPre);
        dynamicsWindow = window;
    } else {
        return;
    }

    dynamicsFocused = true;
    closeEqWindow();
    hideDockedEditor();
    editorHolder.addAndMakeVisible(dynamicsWindow);
    refreshDynamicsButtons();
    resized();
}

void RackView::closeDynamicsWindow() {
    if (dynamicsWindow == nullptr) return;
    auto* doomed = dynamicsWindow;
    dynamicsWindow = nullptr;
    dynamicsFocused = false;
    if (auto* ch = currentChannel()) {
        ch->analyzerTap.setMode(AudioEngine::AnalyzerTap::kOff);
    } else if (auto* out = currentOutput()) {
        out->analyzerTap.setMode(AudioEngine::AnalyzerTap::kOff);
    }
    editorHolder.removeChildComponent(doomed);
    delete doomed;
    refreshDynamicsButtons();
    showDockedEditor();
}

void RackView::toggleDynamicsBypass(pluginrack::DynamicsEffectType type) {
    if (auto* ch = currentChannel()) {
        auto effects = ch->effects;
        const auto found = std::find_if(effects.begin(), effects.end(),
                                        [type](const auto& effect) { return effect.type == type; });
        if (found == effects.end()) return;
        found->bypassed = !found->bypassed;
        audioEngine.setChannelEffects(ch->id, effects);
    } else if (auto* out = currentOutput()) {
        auto effects = out->effects;
        const auto found = std::find_if(effects.begin(), effects.end(),
                                        [type](const auto& effect) { return effect.type == type; });
        if (found == effects.end()) return;
        found->bypassed = !found->bypassed;
        audioEngine.setOutputEffects(out->id, effects);
    } else {
        return;
    }
    refreshDynamicsButtons();
}

void RackView::refreshDynamicsButtons() {
    const auto apply = [this](EqButton& button, pluginrack::DynamicsEffectType type) {
        const std::vector<pluginrack::DynamicsEffect>* effects = nullptr;
        if (auto* ch = currentChannel()) effects = &ch->effects;
        else if (auto* out = currentOutput()) effects = &out->effects;
        bool active = false;
        if (effects != nullptr) {
            const auto found = std::find_if(effects->begin(), effects->end(),
                                            [type](const auto& effect) { return effect.type == type; });
            active = found != effects->end() && !found->bypassed;
            if (found != effects->end() && dynamicsWindow != nullptr &&
                dynamicsWindow->getType() == type) {
                dynamicsWindow->setBypassed(found->bypassed);
                if (type == pluginrack::DynamicsEffectType::Compressor) {
                    const auto gateIt = std::find_if(
                        effects->begin(), effects->end(), [](const auto& effect) {
                            return effect.type == pluginrack::DynamicsEffectType::Gate;
                        });
                    if (gateIt != effects->end()) dynamicsWindow->setGate(*gateIt);
                }
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

void RackView::SourceRowList::setRows(juce::StringArray channelNames,
                                      juce::StringArray deviceNames) {
    if (names == channelNames && devices == deviceNames) return;
    names = std::move(channelNames);
    devices = std::move(deviceNames);
    repaint();
}

void RackView::SourceRowList::paint(juce::Graphics& g) {
    if (names.isEmpty()) {
        g.setColour(theme::dim());
        g.setFont(theme::ui::captionFont());
        g.drawFittedText("Nothing is routed to this output yet.",
                         getLocalBounds().removeFromTop(60), juce::Justification::centredTop, 3);
        return;
    }

    auto area = getLocalBounds();
    for (int i = 0; i < names.size() && area.getHeight() > 0; ++i) {
        auto row = area.removeFromTop(kRowH);
        g.setColour(theme::control().withAlpha(0.6f));
        g.fillRoundedRectangle(row.toFloat().reduced(0.0f, 1.0f), 3.0f);

        auto text = row.reduced(6, 3);
        g.setColour(theme::text());
        g.setFont(theme::ui::bodyBoldFont());
        g.drawFittedText(names[i], text.removeFromTop(15), juce::Justification::centredLeft, 1);
        g.setColour(theme::dim());
        g.setFont(theme::ui::captionFont());
        g.drawFittedText(devices[i], text, juce::Justification::centredLeft, 1);
        area.removeFromTop(2);
    }
}

void RackView::refreshSourceList() {
    juce::StringArray sourceNames;
    juce::StringArray sourceDevices;
    if (const auto* out = currentOutput()) {
        for (const auto& channel : audioEngine.getChannels()) {
            if (audioEngine.getSendGain(channel->id, out->id) <= -30.0f) continue;
            sourceNames.add(channel->name);
            const auto device = audioEngine.getChannelDeviceState(channel->id).deviceName;
            sourceDevices.add(device.isNotEmpty() ? device : juce::String("No input device"));
        }
    }
    sourceList.setRows(std::move(sourceNames), std::move(sourceDevices));
}

void RackView::refreshOutputButtons() {
    outputButtons.clear();
    const auto* ch = currentChannel();
    const auto* out = currentOutput();
    const auto& outputs = audioEngine.getOutputs();

    // With an output selected the column shows its inputs instead of the sends
    // (an output has no sends of its own): which channels feed it, and the
    // device each of those channels is receiving from.
    refreshSourceList();
    outputHeader.setText(out != nullptr ? "Inputs" : "Output", juce::dontSendNotification);

    for (size_t i = 0; i < outputs.size(); ++i) {
        auto* btn = outputButtons.add(new juce::TextButton(juce::String((int)i + 1)));
        btn->setClickingTogglesState(true);
        btn->setTooltip(outputs[i]->name);
        btn->setColour(juce::TextButton::buttonColourId, theme::control());
        btn->setColour(juce::TextButton::buttonOnColourId, theme::amber());
        btn->setColour(juce::TextButton::textColourOffId, theme::text());
        const std::uint32_t outId = outputs[i]->id;
        // initial state
        bool on = false;
        if (ch != nullptr) {
            const float g = audioEngine.getSendGain((std::uint32_t)selectedId, outId);
            on = g > -30.0f;
        }
        btn->setToggleState(on, juce::dontSendNotification);
        const int idx = (int)i;
        btn->onClick = [this, ch, outId, idx] {
            if (currentChannel() == nullptr) return;
            auto* b = outputButtons[idx];
            const bool on = b->getToggleState();
            audioEngine.setSendGain((std::uint32_t)selectedId, outId, on ? 0.0f : -60.0f);
        };
        addAndMakeVisible(*btn);
    }
    // disable when output selected
    const bool isChannel = (currentChannel() != nullptr);
    for (auto* b : outputButtons) b->setEnabled(isChannel);
    for (auto* b : outputButtons) b->setVisible(isChannel);
    sourceList.setVisible(!isChannel && selectedId >= 0);
    repaint();
}

void RackView::rebuild() {
    ensureSelection();
    const bool hasChain = selectedId >= 0;
    emptyLabel.setVisible(!hasChain);
    touchBar.setVisible(hasChain);

    nameLabel.setVisible(hasChain);
    subLabel.setVisible(hasChain);
    gainCaption.setVisible(hasChain);
    gainSlider.setVisible(hasChain);
    inputChip.setVisible(hasChain && currentChannel() != nullptr);
    modeCombo.setVisible(false);
    inputComboA.setVisible(false);
    inputComboB.setVisible(false);
    muteButton.setVisible(hasChain);
    soloButton.setVisible(hasChain && currentChannel() != nullptr);
    mixerButton.setVisible(hasChain);
    channelCombo.setVisible(true);
    insertsHeader.setVisible(hasChain);
    fxHeader.setVisible(hasChain);
    eqButton.setVisible(hasChain);
    compressorButton.setVisible(hasChain);
    gateButton.setVisible(hasChain);
    slotViewport.setVisible(hasChain);
    editorViewport.setVisible(hasChain);
    outputHeader.setVisible(hasChain);
    for (auto* b : outputButtons) b->setVisible(hasChain);
    sourceList.setVisible(false);

    if (!hasChain) {
        closeEqWindow();
        closeDynamicsWindow();
        closeDockedEditor();
        slots.clear();
        outputButtons.clear();
        resized();
        return;
    }

    rebuildChannelCombo();
    refreshHeader();
    refreshSlots();
    refreshEditor();
    refreshOutputButtons();
    refreshDynamicsButtons();
    resized();
}

void RackView::resized() {
    auto area = getLocalBounds().reduced(8);
    if (emptyLabel.isVisible()) {
        emptyLabel.setBounds(area);
        return;
    }

    const int leftW = 160;
    const int pluginW = 220;
    const int rightW = 110;
    const int touchW = 64;
    const int gap = 8;

    auto leftArea = area.removeFromLeft(leftW);
    area.removeFromLeft(gap);
    auto rightArea = area.removeFromRight(rightW);
    area.removeFromRight(gap);
    auto touchArea = area.removeFromRight(touchW);
    area.removeFromRight(gap);
    auto pluginArea = area.removeFromLeft(pluginW);
    area.removeFromLeft(gap);
    auto centerArea = area;

    touchBar.setBounds(touchArea);

    // left column
    auto la = leftArea.reduced(4);
    la.removeFromTop(14);
    auto selectorRow = la.removeFromTop(32);
    channelCombo.setBounds(selectorRow);
    la.removeFromTop(6);
    nameLabel.setBounds(la.removeFromTop(20));
    subLabel.setBounds(la.removeFromTop(16));
    la.removeFromTop(10);

    if (currentChannel() != nullptr) {
        auto inputRow = la.removeFromTop(theme::metrics::kFieldH);
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
        la.removeFromTop(2);
    }

    // Trim and Pan share one row, matching the Mixer channel strip.
    if (currentChannel() != nullptr) {
        auto controlsRow = la.removeFromTop(74);
        const int knobCellW = (controlsRow.getWidth() - 4) / 2;
        auto trimCell = controlsRow.removeFromLeft(knobCellW);
        controlsRow.removeFromLeft(4);
        auto panCell = controlsRow.removeFromLeft(knobCellW);
        trimCaption.setBounds(trimCell.removeFromTop(14));
        trimKnob.setBounds(trimCell.reduced(2));
        panCaption.setBounds(panCell.removeFromTop(14));
        panSlider.setBounds(panCell.reduced(2));
        la.removeFromTop(4);
    }

    la.removeFromBottom(6); // breathing room above the panel edge
    auto btnRow = la.removeFromBottom(24);
    la.removeFromBottom(14);

    auto faderRow = la;
    auto meterColumn = faderRow.removeFromRight(LevelMeter::kPreferredWidth);
    faderRow.removeFromRight(4);
    gainCaption.setBounds(faderRow.removeFromTop(14));
    gainSlider.setBounds(faderRow.reduced(0, 4));
    // The meter lines its bars up with the fader's travel itself, and takes
    // the strip above that travel for the Peak/RMS readouts.
    if (meter != nullptr) meter->setBounds(meterColumn);

    if (soloButton.isVisible()) {
        const int third = btnRow.getWidth() / 3 - 3;
        muteButton.setBounds(btnRow.removeFromLeft(third));
        btnRow.removeFromLeft(4);
        soloButton.setBounds(btnRow.removeFromLeft(third));
        btnRow.removeFromLeft(4);
        mixerButton.setBounds(btnRow);
    } else {
        const int half = btnRow.getWidth() / 2 - 2;
        muteButton.setBounds(btnRow.removeFromLeft(half));
        btnRow.removeFromLeft(4);
        mixerButton.setBounds(btnRow);
    }

    // plugin column
    auto pa = pluginArea.reduced(4);
    pa.removeFromTop(14); // panel caption band
    fxHeader.setBounds(pa.removeFromTop(16));
    pa.removeFromTop(4);
    // Built-in effects and VST inserts share one ordered list in the rack.
    pa.removeFromTop(14);
    insertsHeader.setBounds(pa.removeFromTop(16));
    pa.removeFromTop(4);
    slotViewport.setBounds(pa);
    refreshSlots();

    // Keep embedded editors inside the framed center bay. The viewport and
    // its child editors paint opaque surfaces, so using the panel bounds
    // directly would cover the panel outline.
    const auto editorArea = centerArea.reduced(2);
    editorViewport.setBounds(editorArea);
    const int vw = juce::jmax(0, editorViewport.getWidth());
    const int vh = juce::jmax(0, editorViewport.getHeight());
    if (eqFocused && eqWindow != nullptr) {
        // The EQ fills the editor bay, like the dynamics panel does.
        editorHolder.setSize(juce::jmax(vw, EQWindow::kWidth),
                             juce::jmax(vh, EQWindow::kHeight));
        eqWindow->setBounds(editorHolder.getLocalBounds());
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
    }
    if (dynamicsFocused && dynamicsWindow != nullptr) {
        // Size the holder to the bay, not to whatever the last docked VST
        // editor left behind: the panel's knob rows sit at the bottom and
        // would otherwise fall outside the visible area.
        editorHolder.setSize(juce::jmax(vw, DynamicsWindow::kMinWidth),
                             juce::jmax(vh, DynamicsWindow::kMinHeight));
        dynamicsWindow->setBounds(editorHolder.getLocalBounds());
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
    }
    if (dockedEditor != nullptr) {
        // The bay is the allocation: keep the holder at the visible size; the
        // layout scales the plugin to fit that whole area.
        editorHolder.setSize(juce::jmax(0, vw), juce::jmax(0, vh));
        layoutDockedEditor();
        noEditorLabel.setBounds(editorHolder.getLocalBounds());
    } else {
        editorHolder.setSize(juce::jmax(0, vw), juce::jmax(0, vh));
        auto noArea = editorHolder.getLocalBounds();
        if (noEditorBypass.isVisible()) {
            noEditorBypass.setBounds(noArea.removeFromTop(24).removeFromRight(100));
            noArea.removeFromTop(4);
        }
        noEditorLabel.setBounds(noArea);
    }

    // right column
    auto ra = rightArea.reduced(4);
    ra.removeFromTop(14);
    outputHeader.setBounds(ra.removeFromTop(20));
    ra.removeFromTop(4);
    sourceList.setBounds(ra);
    int y = ra.getY();
    const int btnH = 26;
    const int btnGap = 4;
    for (auto* b : outputButtons) {
        b->setBounds(ra.getX(), y, ra.getWidth(), btnH);
        y += btnH + btnGap;
    }
}

void RackView::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
    if (emptyLabel.isVisible()) return;

    auto area = getLocalBounds().reduced(8);
    const int leftW = 160;
    const int pluginW = 220;
    const int rightW = 110;
    const int touchW = 64;
    const int gap = 8;
    auto leftArea = area.removeFromLeft(leftW);
    area.removeFromLeft(gap);
    auto rightArea = area.removeFromRight(rightW);
    area.removeFromRight(gap);
    auto touchArea = area.removeFromRight(touchW);
    area.removeFromRight(gap);
    auto pluginArea = area.removeFromLeft(pluginW);
    area.removeFromLeft(gap);
    const auto centerArea = area;

    const auto drawPanel = [&g](juce::Rectangle<int> bounds, juce::Colour fill,
                                juce::Colour outline) {
        theme::ui::drawPanel(g, bounds, fill, outline);
    };

    drawPanel(leftArea, theme::panel().darker(0.08f), theme::border());
    drawPanel(pluginArea, theme::panel(), theme::border());
    drawPanel(centerArea, theme::bg().darker(0.16f), theme::line().withAlpha(0.8f));
    drawPanel(rightArea, theme::panel().darker(0.08f), theme::border());
    // The touch strip paints its own panel; this keeps the column framed if it
    // is ever hidden (no chain selected).
    drawPanel(touchArea, theme::panel().darker(0.08f), theme::border());

    theme::ui::drawSectionCaption(g, { leftArea.getX() + 8, leftArea.getY() + 4, 70, 12 }, "INPUT");
    theme::ui::drawSectionCaption(g, { pluginArea.getX() + 8, pluginArea.getY() + 4, 80, 12 }, "INSERTS");
    theme::ui::drawSectionCaption(g, { centerArea.getX() + 10, centerArea.getY() + 6, 110, 12 },
                                  "PLUGIN EDITOR");
    theme::ui::drawSectionCaption(g, { rightArea.getX() + 8, rightArea.getY() + 4, 70, 12 },
                                  selectedIsOutput ? "Inputs" : "Output");
}

void RackView::jumpToMixer() {
    if (onJumpToMixer) onJumpToMixer();
}

void RackView::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isRightButtonDown() && e.originalComponent == &nameLabel && selectedId >= 0) {
        renameSelectedChain();
        return;
    }
    juce::Component::mouseDown(e);
}

void RackView::renameSelectedChain() {
    juce::String currentName;
    if (auto* ch = currentChannel()) currentName = ch->name;
    else if (auto* out = currentOutput()) currentName = out->name;

    auto* aw = new juce::AlertWindow("Rename", "Enter new name:", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor("name", currentName, "Name:");
    aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<RackView> safe(this);
    aw->enterModalState(true, juce::ModalCallbackFunction::create([safe, aw](int result) {
        if (result == 1 && safe != nullptr) {
            const auto newName = aw->getTextEditorContents("name").trim();
            if (newName.isNotEmpty()) {
                if (auto* c = safe->currentChannel())
                    safe->audioEngine.setChannelName(c->id, newName);
                else if (auto* o = safe->currentOutput())
                    safe->audioEngine.setOutputName(o->id, newName);
                safe->rebuild();
            }
        }
        delete aw;
    }), true);
}

juce::String RackView::insertPlugin(const juce::File& file, int pluginUniqueId) {
    // The load runs on the message thread because that is the only thread these
    // plugins tolerate, and some of them spend seconds initialising (Native
    // Instruments' Raum needs ~3.5 s). Show a wait cursor now so the window
    // explains the stall instead of just freezing.
    juce::Component::SafePointer<RackView> safe(this);
    juce::MouseCursor::showWaitCursor();
    auto finish = [safe](juce::String error) {
        if (safe != nullptr) {
            safe->refreshAfterInsertChange();
            if (error.isNotEmpty()) {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon, "PluginRack", error);
            }
        }
        juce::MouseCursor::hideWaitCursor();
    };
    if (auto* ch = currentChannel()) {
        const auto channelId = ch->id;
        audioEngine.addPluginAsync(channelId, file, pluginUniqueId, finish);
        return {};
    }
    if (auto* out = currentOutput()) {
        const auto outputId = out->id;
        audioEngine.addOutputPluginAsync(outputId, file, pluginUniqueId, finish);
        return {};
    }
    juce::MouseCursor::hideWaitCursor();
    return "No chain is selected.";
}

void RackView::erasePlugin(std::uint32_t insertId) {
    // The docked editor always belongs to the selected chain, which is the
    // chain this erase targets: release it while the processor is still
    // alive, whichever slot held the focus. Erasing first would leave
    // closeDockedEditor deleting an editor whose processor is already gone.
    closeDockedEditor();
    if (auto* ch = currentChannel()) audioEngine.removePlugin(ch->id, insertId);
    else if (auto* out = currentOutput()) audioEngine.removeOutputPlugin(out->id, insertId);
}

void RackView::setPluginBypass(std::uint32_t insertId, bool bypassed) {
    if (auto* ch = currentChannel()) audioEngine.setPluginBypassed(ch->id, insertId, bypassed);
    else if (auto* out = currentOutput()) audioEngine.setOutputPluginBypassed(out->id, insertId, bypassed);
}

void RackView::reorderInsert(std::uint32_t insertId, int targetIndex) {
    if (selectedIsOutput)
        audioEngine.moveOutputChainItem(selectedId, {pluginrack::ChainItemType::Insert, insertId},
                                        targetIndex);
    else
        audioEngine.moveChannelChainItem(selectedId, {pluginrack::ChainItemType::Insert, insertId},
                                         targetIndex);
    juce::Component::SafePointer<RackView> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        safe->chainDropTarget = -1;
        safe->refreshSlots();
        if (safe->onStructureChanged) safe->onStructureChanged();
    });
}

int RackView::chainBoundaryY(int targetIndex) const {
    if (chainRowCount <= 0) return 2;
    const int target = juce::jlimit(0, chainRowCount, targetIndex);
    if (target < chainRowCount) return juce::jmax(0, slots[target]->getY() - 3);
    return slots[chainRowCount - 1]->getBottom() + 1;
}

int RackView::chainDropIndexAt(juce::Point<int> parentPoint) {
    int index = 0;
    for (int i = 0; i < chainRowCount; ++i)
        if (parentPoint.y > slots[i]->getBounds().getCentreY()) index = i + 1;
    return index;
}

void RackView::previewChainItem(int targetIndex) {
    chainDropTarget = juce::jlimit(0, chainRowCount, targetIndex);
    chainMarker.setBounds(4, chainBoundaryY(chainDropTarget),
                          juce::jmax(0, slotViewport.getWidth() - 8), 2);
    chainMarker.setVisible(true);
    chainMarker.toFront(false);
}

void RackView::clearChainPreview() {
    chainDropTarget = -1;
    chainMarker.setVisible(false);
}

void RackView::moveChainItemRaw(pluginrack::ChainItem item, int chainIndex) {
    if (selectedIsOutput)
        audioEngine.moveOutputChainItem(selectedId, item, chainIndex);
    else
        audioEngine.moveChannelChainItem(selectedId, item, chainIndex);
}

void RackView::moveChainCard(const std::vector<pluginrack::ChainItem>& items, int targetRowIndex) {
    if (items.empty()) return;
    const auto* chain = currentChain();
    if (chain == nullptr) return;
    const auto* effects = currentEffects();
    static const std::vector<pluginrack::DynamicsEffect> fallbackEffects =
        pluginrack::defaultDynamicsEffects();
    const auto rows =
        pluginrack::buildChainRows(*chain, effects != nullptr ? *effects : fallbackEffects);

    // Plan the whole card against a simulated chain: the engine erases each
    // item before inserting it, so the followers must not be shifted by one
    // when the card drops below its own rows.
    const auto moves = pluginrack::planChainCardMoves(*chain, rows, targetRowIndex, items);
    for (const auto& move : moves) moveChainItemRaw(move.item, move.targetIndex);
}

void RackView::reorderChainItem(pluginrack::ChainItem item, int targetIndex) {
    // A card moves as one, so collect every chain entry it covers.
    std::vector<pluginrack::ChainItem> cardItems{ item };
    if (const auto* chain = currentChain()) {
        if (const auto* effects = currentEffects()) {
            for (const auto& row : pluginrack::buildChainRows(*chain, *effects)) {
                const bool holdsItem =
                    std::any_of(row.begin(), row.end(), [&](int index) {
                        return (*chain)[static_cast<std::size_t>(index)] == item;
                    });
                if (!holdsItem) continue;
                if (row.size() > 1) {
                    cardItems.clear();
                    for (const int index : row)
                        cardItems.push_back((*chain)[static_cast<std::size_t>(index)]);
                }
                break;
            }
        }
    }
    moveChainCard(cardItems, targetIndex);
    // The slot that reported this is still inside its own mouseUp() call, so
    // rebuilding the rows now would delete the component mid-event.
    juce::Component::SafePointer<RackView> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        auto callback = safe->onStructureChanged;
        safe->chainDropTarget = -1;
        safe->chainMarker.setVisible(false);
        safe->refreshSlots();
        if (callback) callback();
    });
}

void RackView::setBuiltInBypass(pluginrack::ChainItem item, bool bypassed) {
    if (item.type == pluginrack::ChainItemType::Eq) {
        if (selectedIsOutput) {
            const auto& outputs = audioEngine.getOutputs();
            for (int i = 0; i < static_cast<int>(outputs.size()); ++i)
                if (outputs[static_cast<std::size_t>(i)]->id == static_cast<std::uint32_t>(selectedId))
                    audioEngine.setOutputEqBypassed(i, bypassed);
        } else {
            audioEngine.setChannelEqBypassed(static_cast<std::uint32_t>(selectedId), bypassed);
        }
    } else {
        // Resolve like the card does: a scene keeps the effect ids it was
        // saved with, which can differ from the chain row's id.
        const auto* resolved = dynamicsEffectFor(item.id);
        const auto targetId = resolved != nullptr ? resolved->id : item.id;
        std::vector<pluginrack::DynamicsEffect> effects;
        std::uint32_t ownerId = 0;
        if (selectedIsOutput) {
            if (auto* output = currentOutput()) {
                effects = output->effects;
                ownerId = output->id;
            }
        } else if (auto* channel = currentChannel()) {
            effects = channel->effects;
            ownerId = channel->id;
        }
        if (ownerId == 0) return;

        for (auto& effect : effects)
            if (effect.id == targetId) effect.bypassed = bypassed;
        if (selectedIsOutput) audioEngine.setOutputEffects(ownerId, effects);
        else audioEngine.setChannelEffects(ownerId, effects);

        // The panel is open on these same effects, so its switches follow the
        // card instead of going stale until it is reopened. Guarded on the
        // page type: a gate page's own effect is the gate, not the compressor.
        if (dynamicsWindow != nullptr) {
            for (const auto& effect : effects) {
                if (effect.type == pluginrack::DynamicsEffectType::Compressor
                    && dynamicsWindow->getType() == pluginrack::DynamicsEffectType::Compressor)
                    dynamicsWindow->setBypassed(effect.bypassed);
                else if (effect.type == pluginrack::DynamicsEffectType::Gate)
                    dynamicsWindow->setGate(effect);
            }
        }
    }
}

void RackView::activateBuiltIn(pluginrack::ChainItem item) {
    if (item.type == pluginrack::ChainItemType::Eq) focusEq();
    // Both dynamics rows open the combined panel: it is the only page that
    // carries the gate row and its curve.
    else focusDynamics(pluginrack::DynamicsEffectType::Compressor);
}

void RackView::refreshAfterInsertChange() {
    const auto* inserts = currentInserts();
    bool found = false;
    if (inserts != nullptr) {
        for (const auto& insert : *inserts) {
            if (insert->id == focusedInsertId) {
                found = true;
                break;
            }
        }
    }
    if (!found) {
        focusedInsertId = (inserts != nullptr && !inserts->empty()) ? inserts->back()->id : 0;
    }
    refreshSlots();
    refreshEditor();
    juce::Component::SafePointer<RackView> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        auto callback = safe->onStructureChanged;
        if (callback) callback();
    });
}

bool RackView::insertActivated(AudioEngine::InsertSlot* insert) {
    if (insert == nullptr) return false;
    eqFocused = false;
    if (eqWindow != nullptr) {
        auto* doomed = eqWindow;
        eqWindow = nullptr;
        editorHolder.removeChildComponent(doomed);
        delete doomed;
        refreshEqButton();
    }
    // Clicking a slot opens that insert in the bay, so the compressor panel
    // has to give it up instead of staying underneath.
    closeDynamicsWindow();
    focusedInsertId = insert->id;
    refreshEditor();
    return true;
}
