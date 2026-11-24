#include "RackView.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "EffectEdits.h"
#include "MultibandCurveComponent.h"
#include "NativeThumbnail.h"

namespace {
constexpr int kSlotGap = 4;
constexpr int kSlotInset = 4;
// Newest samples the dynamics thumbnails measure their level over; same
// window the dynamics panel uses.
constexpr int kLevelFrames = 512;
} // namespace

RackView::RackView(AudioEngine& engine) : audioEngine(engine), sideChainBar(engine) {
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
    styleSectionHeader(insertsHeader, "Inserts");

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

    addChildComponent(sideChainBar);

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
    closeEffectWindows();
    closeEqWindow();
    closeDynamicsWindow();
    // Stop the capture worker before the editor it captures goes away.
    thumbnailWorker.stop();
    clearHiddenPreviews();
    closeDockedEditor();
    if (previewHostWindow != nullptr) {
        nativedisplay::destroyHiddenHost(previewHostWindow);
        previewHostWindow = nullptr;
    }
}

std::uint32_t RackView::selectedChainId() const {
    return selectedId < 0 ? 0 : static_cast<std::uint32_t>(selectedId);
}

void RackView::setSelectedChannel(int channelId, bool isOutput) {
    if (channelId == selectedId && isOutput == selectedIsOutput && channelId >= 0) return;
    closeEffectWindows();
    closeEqWindow();
    // The dynamics panel is bound to the strip it was opened on; leaving it up
    // would push its edits into whatever strip becomes selected next.
    closeDynamicsWindow();
    hiddenCursor = 0;
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
    const auto id = static_cast<std::uint32_t>(selectedId);
    if (auto* out = audioEngine.getOutputById(id)) return out;
    return audioEngine.getBusById(id);
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
        for (const auto& bus : audioEngine.getBuses())
            if (bus->id == id) return &bus->inserts;
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
        ok = selectedIsOutput
                 ? (audioEngine.getOutputById(id) != nullptr
                    || audioEngine.getBusById(id) != nullptr)
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
    for (const auto& bus : audioEngine.getBuses()) {
        channelCombo.addItem(bus->name + " (bus)", item++);
        comboIds.push_back((int)bus->id);
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

    // Fader in the selected chain's own identity colour.
    const float hue = ch != nullptr ? ch->colourHue
                                  : (out != nullptr ? out->colourHue : 0.0f);
    const auto accent = theme::stripColour(hue);
    gainSlider.setColour(juce::Slider::trackColourId, accent);
    gainSlider.setColour(juce::Slider::thumbColourId, accent);

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
        if (isSelectedBus()) {
            subLabel.setText("Bus - summed from its sends", juce::dontSendNotification);
        } else {
            const auto deviceName = audioEngine.getOutputDeviceState(out->id).deviceName;
            subLabel.setText(deviceName.isNotEmpty() ? deviceName : juce::String("No device"),
                             juce::dontSendNotification);
        }
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
    multibandCard = nullptr;
    deEsserCard = nullptr;
    limiterCard = nullptr;
    dynamicsCard = nullptr;
    dynamicsCardPaired = false;
    vstCards.clear();
    const auto* inserts = currentInserts();
    const auto* chain = selectedIsOutput
                            ? (currentOutput() == nullptr ? nullptr : &currentOutput()->chain)
                            : (currentChannel() == nullptr ? nullptr : &currentChannel()->chain);
    // Empty chain means no built-ins loaded: draw nothing, not a placeholder
    // EQ/Dynamics pair. New channels start with defaultChain() == {}.
    const std::vector<pluginrack::ChainItem> noItems;
    const auto& items = chain != nullptr ? *chain : noItems;
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
            slot->setInsert(found, liveRateHz);
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
        } else if (item.type == pluginrack::ChainItemType::Multiband || item.type == pluginrack::ChainItemType::DeEsser || item.type == pluginrack::ChainItemType::Limiter || item.type == pluginrack::ChainItemType::FeedbackSuppressor || item.type == pluginrack::ChainItemType::SpectralDenoiser) {
            const juce::String name = item.type == pluginrack::ChainItemType::Multiband ? "Multiband Comp" : item.type == pluginrack::ChainItemType::DeEsser ? "De-esser" : item.type == pluginrack::ChainItemType::Limiter ? "Limiter" : item.type == pluginrack::ChainItemType::SpectralDenoiser ? "Spectral Denoiser" : "Feedback Suppressor";
            bool bp = false;
            if (auto* ch = currentChannel()) {
                if (item.type == pluginrack::ChainItemType::Multiband) for (auto& e : ch->multibandEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::DeEsser) for (auto& e : ch->deEsserEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::Limiter) for (auto& e : ch->limiterEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::FeedbackSuppressor) for (auto& e : ch->feedbackSuppressorEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::SpectralDenoiser) for (auto& e : ch->spectralDenoiserEffects) if (e.id==item.id) bp=e.bypassed;
            } else if (auto* out = currentOutput()) {
                if (item.type == pluginrack::ChainItemType::Multiband) for (auto& e : out->multibandEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::DeEsser) for (auto& e : out->deEsserEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::Limiter) for (auto& e : out->limiterEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::FeedbackSuppressor) for (auto& e : out->feedbackSuppressorEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::SpectralDenoiser) for (auto& e : out->spectralDenoiserEffects) if (e.id==item.id) bp=e.bypassed;
            }
            slot->setBuiltIn(item, name, bp);
            if (item.type == pluginrack::ChainItemType::Multiband) {
                auto mini = std::make_unique<InsertMiniView>();
                const auto* selectedEffect = [&] {
                    if (selectedChannel != nullptr) {
                        for (const auto& e : selectedChannel->multibandEffects)
                            if (e.id == item.id) return &e;
                    } else if (selectedOutput != nullptr) {
                        for (const auto& e : selectedOutput->multibandEffects)
                            if (e.id == item.id) return &e;
                    }
                    return static_cast<const pluginrack::MultibandEffect*>(nullptr);
                }();
                if (selectedEffect != nullptr) mini->showMultiband(*selectedEffect, liveRateHz);
                slot->setMiniView(std::move(mini));
                slot->setSelected(multibandWindow != nullptr);
                multibandCard = slot;
            } else if (item.type == pluginrack::ChainItemType::DeEsser) {
                auto mini = std::make_unique<InsertMiniView>();
                const auto* selectedEffect = [&] {
                    if (selectedChannel != nullptr) {
                        for (const auto& e : selectedChannel->deEsserEffects)
                            if (e.id == item.id) return &e;
                    } else if (selectedOutput != nullptr) {
                        for (const auto& e : selectedOutput->deEsserEffects)
                            if (e.id == item.id) return &e;
                    }
                    return static_cast<const pluginrack::DeEsserEffect*>(nullptr);
                }();
                if (selectedEffect != nullptr) mini->showDeEsser(*selectedEffect, liveRateHz);
                slot->setMiniView(std::move(mini));
                slot->setSelected(deEsserWindow != nullptr);
                deEsserCard = slot;
            } else if (item.type == pluginrack::ChainItemType::Limiter) {
                auto mini = std::make_unique<InsertMiniView>();
                const auto* selectedEffect = [&] {
                    if (selectedChannel != nullptr) {
                        for (const auto& e : selectedChannel->limiterEffects)
                            if (e.id == item.id) return &e;
                    } else if (selectedOutput != nullptr) {
                        for (const auto& e : selectedOutput->limiterEffects)
                            if (e.id == item.id) return &e;
                    }
                    return static_cast<const pluginrack::LimiterEffect*>(nullptr);
                }();
                if (selectedEffect != nullptr) mini->showLimiter(*selectedEffect);
                slot->setMiniView(std::move(mini));
                slot->setSelected(limiterWindow != nullptr);
                limiterCard = slot;
            }
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

    // The add row is the last row: whenever there is budget left there is
    // somewhere to click for the next plugin. At the global budget the add
    // row disappears so the picker can never open against a refusal.
    const bool canAddVst =
        inserts == nullptr || static_cast<int>(inserts->size()) < kChainVstSlots;
    const int totalRows = juce::jmax(chainRowCount, canAddVst ? chainRowCount + 1 : 0);
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
    // Strip and bus lists change with the scene, so the side-chain bar re-reads
    // them whenever the cards are rebuilt.
    if (sideChainBarVisible) sideChainBar.refresh();
    startMiniTimer();
}

void RackView::layoutSlots() {
    // Re-place the existing cards after a resize. Rebuilding them here would
    // throw away every mini view (and its cached thumbnail) on any window
    // resize, and it is the expensive part of laying the dock out.
    const int width = juce::jmax(0, slotViewport.getWidth() - 2 * kSlotInset);
    int y = kSlotInset;
    for (auto* slot : slots) {
        if (slot == nullptr) continue;
        slot->setBounds(kSlotInset, y, width, slot->preferredHeight());
        y += slot->preferredHeight() + kSlotGap;
    }
    // Selecting an insert does not rebuild the cards (resized() only calls this
    // layout), so the highlight has to be refreshed here or the click appears to
    // do nothing until the next structural change.
    for (const auto& [id, card] : vstCards)
        if (card != nullptr)
            card->setSelected(dockedHost != nullptr && focusedInsertId == id);
    slotContent.setSize(juce::jmax(0, slotViewport.getWidth()), juce::jmax(0, y));
    if (chainDropTarget >= 0) {
        chainMarker.setBounds(kSlotInset, chainBoundaryY(chainDropTarget), width, 2);
        chainMarker.setVisible(true);
        chainMarker.toFront(false);
    } else {
        chainMarker.setVisible(false);
    }
}

void RackView::visibilityChanged() {
    if (isShowing()) startMiniTimer();
    else {
        stopTimer();
        releaseMiniTap();
    }
}

AudioEngine::AnalyzerTap* RackView::currentTap() {
    // Mini previews read the strip-end legacy tap; each card's *editor*
    // reads that plugin's own per-plugin tap (eqTap/multibandTap/...).
    if (auto* channel = currentChannel()) return &channel->analyzerTap;
    if (auto* output = currentOutput()) return &output->analyzerTap;
    return nullptr;
}

AudioEngine::AnalyzerTap* RackView::currentPluginTap(pluginrack::ChainItemType type) {
    // Each built-in's independent RTA: the Multiband preview reads what
    // leaves the Multiband, the EQ preview reads what leaves the EQ, etc.
    // A later plugin in the chain can no longer tint an earlier RTA.
    if (auto* channel = currentChannel()) {
        switch (type) {
            case pluginrack::ChainItemType::Eq: return &channel->eqTap;
            case pluginrack::ChainItemType::Multiband: return &channel->multibandTap;
            case pluginrack::ChainItemType::DeEsser: return &channel->deEsserTap;
            case pluginrack::ChainItemType::Limiter: return &channel->limiterTap;
            case pluginrack::ChainItemType::Dynamics: return &channel->dynamicsTap;
            default: return &channel->analyzerTap;
        }
    }
    if (auto* output = currentOutput()) {
        switch (type) {
            case pluginrack::ChainItemType::Eq: return &output->eqTap;
            case pluginrack::ChainItemType::Multiband: return &output->multibandTap;
            case pluginrack::ChainItemType::DeEsser: return &output->deEsserTap;
            case pluginrack::ChainItemType::Limiter: return &output->limiterTap;
            case pluginrack::ChainItemType::Dynamics: return &output->dynamicsTap;
            default: return &output->analyzerTap;
        }
    }
    return nullptr;
}

void RackView::releaseMiniTap() {
    // Resolved by id, never by pointer: a scene restore destroys the strip the
    // tap lived on, so a stored pointer could outlive it.
    if (tapArmedId == 0) return;
    if (tapArmedIsOutput) {
        const auto release = [this](auto& strips) {
            for (const auto& strip : strips)
                if (strip->id == tapArmedId) {
                    strip->analyzerTap.setMode(AudioEngine::AnalyzerTap::kOff);
                    strip->eqTap.setMode(AudioEngine::AnalyzerTap::kOff);
                    strip->dynamicsTap.setMode(AudioEngine::AnalyzerTap::kOff);
                    strip->multibandTap.setMode(AudioEngine::AnalyzerTap::kOff);
                    strip->deEsserTap.setMode(AudioEngine::AnalyzerTap::kOff);
                    strip->limiterTap.setMode(AudioEngine::AnalyzerTap::kOff);
                }
        };
        release(audioEngine.getOutputs());
        release(audioEngine.getBuses());
    } else {
        for (const auto& channel : audioEngine.getChannels())
            if (channel->id == tapArmedId) {
                channel->analyzerTap.setMode(AudioEngine::AnalyzerTap::kOff);
                channel->eqTap.setMode(AudioEngine::AnalyzerTap::kOff);
                channel->dynamicsTap.setMode(AudioEngine::AnalyzerTap::kOff);
                channel->multibandTap.setMode(AudioEngine::AnalyzerTap::kOff);
                channel->deEsserTap.setMode(AudioEngine::AnalyzerTap::kOff);
                channel->limiterTap.setMode(AudioEngine::AnalyzerTap::kOff);
            }
    }
    tapArmedId = 0;
}

void RackView::startMiniTimer() {
    const bool hasMinis = eqCard != nullptr || dynamicsCard != nullptr
        || multibandCard != nullptr || deEsserCard != nullptr || limiterCard != nullptr
        || !vstCards.empty();
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
        // The preview host is a window of its own; leaving it shown while this
        // view is hidden puts a black rectangle over the desktop.
        if (previewHostWindow != nullptr)
            nativedisplay::destroyHiddenHost(previewHostWindow), previewHostWindow = nullptr;
        return;
    }
    // JUCE re-syncs a plugin's native window whenever the plugin resizes itself;
    // keep it parked inside the bay.
    if (dockedEditor != nullptr) layoutDockedEditor();
    // For a couple of seconds after an editor change, a plugin may create a
    // natural-size window parked above the app; hide it before the OS paints
    // its white surface over the top bar.
    if (juce::Time::getMillisecondCounterHiRes() < offscreenGuardUntilMs)
        hideOffscreenPluginWindows();
    // A plugin can re-show the previous editor's native window a moment after
    // the swap, leaving it beside the new one. Keep only the docked editor's
    // window visible inside the app for a few seconds after every switch.
    if (dockedEditor != nullptr && !swappingEditor
        && juce::Time::getMillisecondCounterHiRes() < swapGuardUntilMs) {
        if (auto* peer = getPeer()) {
            const auto wr = nativedisplay::windowRect(peer->getNativeHandle());
            const int hidden = nativedisplay::hideJuceWindowsInRect(
                peer->getNativeHandle(), nativedisplay::pluginWindowOf(*dockedEditor),
                nullptr, wr.getX(), wr.getY(), wr.getRight(), wr.getBottom());
            if (hidden > 0) {
                juce::Logger::writeToLog("rack: guard hid " + juce::String(hidden));
                swapGuardUntilMs = juce::Time::getMillisecondCounterHiRes() + 1000.0;
            }
        }
    }
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

    // Built-in thumbnails each read their own plugin's tap, so the EQ
    // card can't show the Multiband's output and vice versa. Re-armed
    // every tick because closing another view's panel switches taps off.
    auto* tap = (eqCard != nullptr || multibandCard != nullptr || deEsserCard != nullptr
                 || limiterCard != nullptr || dynamicsCard != nullptr)
                    ? currentTap()
                    : nullptr;
    auto* eqTap = eqCard != nullptr ? currentPluginTap(pluginrack::ChainItemType::Eq) : nullptr;
    auto* mbTap = multibandCard != nullptr ? currentPluginTap(pluginrack::ChainItemType::Multiband) : nullptr;
    auto* dsTap = deEsserCard != nullptr ? currentPluginTap(pluginrack::ChainItemType::DeEsser) : nullptr;
    auto* limTap = limiterCard != nullptr ? currentPluginTap(pluginrack::ChainItemType::Limiter) : nullptr;
    if (tap != nullptr) {
        // Selecting another strip would otherwise leave the previous one
        // copying every block into its rings for the rest of the session.
        if (tapArmedId != 0
            && (tapArmedId != selectedChainId() || tapArmedIsOutput != selectedIsOutput)) {
            releaseMiniTap();
        }
        tap->setMode(AudioEngine::AnalyzerTap::kPost);
        if (eqTap != nullptr) eqTap->setMode(AudioEngine::AnalyzerTap::kPost);
        if (mbTap != nullptr) mbTap->setMode(AudioEngine::AnalyzerTap::kPost);
        if (dsTap != nullptr) dsTap->setMode(AudioEngine::AnalyzerTap::kPost);
        if (limTap != nullptr) limTap->setMode(AudioEngine::AnalyzerTap::kPost);
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
        (eqTap != nullptr ? eqTap : tap)->snapshot(true, tapLeft.data(), tapRight.data(), MiniSpectrum::kFftSize);
        miniSpectrum.refresh(tapLeft.data(), tapRight.data());
        if (auto* mini = eqCard->miniView())
            mini->setSpectrum(miniSpectrum.magnitudes(), miniSpectrum.numBins());
    }

    if (tap != nullptr && (multibandCard != nullptr || deEsserCard != nullptr)) {
        // Each card snapshots its own plugin's tap, so the Multiband and
        // De-esser RTAs can differ as soon as their settings differ.
        if (mbTap != nullptr) mbTap->snapshot(true, tapLeft.data(), tapRight.data(), MiniSpectrum::kFftSize);
        else if (tap != nullptr) tap->snapshot(true, tapLeft.data(), tapRight.data(), MiniSpectrum::kFftSize);
        float peak = 0.0f;
        for (int i = 0; i < MiniSpectrum::kFftSize; ++i)
            peak = std::max(peak, std::max(std::abs(tapLeft[static_cast<std::size_t>(i)]),
                                           std::abs(tapRight[static_cast<std::size_t>(i)])));
        const float levelDb = 20.0f * std::log10(std::max(peak, 1.0e-6f));
        if (multibandCard != nullptr) {
            multibandSpectrum.refresh(tapLeft.data(), tapRight.data());
            const auto* effect = [&] {
                if (auto* channel = currentChannel()) {
                    for (const auto& e : channel->multibandEffects)
                        if (!e.bypassed) return &e;
                } else if (auto* output = currentOutput()) {
                    for (const auto& e : output->multibandEffects)
                        if (!e.bypassed) return &e;
                }
                return static_cast<const pluginrack::MultibandEffect*>(nullptr);
            }();
            if (auto* mini = multibandCard->miniView()) {
                mini->setSpectrum(multibandSpectrum.magnitudes(), multibandSpectrum.numBins());
                if (effect != nullptr) {
                    mini->setMultiband(*effect, liveRateHz);
                    std::vector<float> reductions;
                    reductions.reserve(effect->bands.size());
                    for (const auto& band : effect->bands)
                        reductions.push_back(std::max(0.0f, -MultibandCurveComponent::compressionGainDb(
                            levelDb, band)));
                    mini->setGainReduction(reductions);
                }
            }
        }
        if (deEsserCard != nullptr) {
            if (dsTap != nullptr) dsTap->snapshot(true, tapLeft.data(), tapRight.data(), MiniSpectrum::kFftSize);
            deEsserSpectrum.refresh(tapLeft.data(), tapRight.data());
            const auto* effect = [&] {
                if (auto* channel = currentChannel()) {
                    for (const auto& e : channel->deEsserEffects)
                        if (!e.bypassed) return &e;
                } else if (auto* output = currentOutput()) {
                    for (const auto& e : output->deEsserEffects)
                        if (!e.bypassed) return &e;
                }
                return static_cast<const pluginrack::DeEsserEffect*>(nullptr);
            }();
            if (auto* mini = deEsserCard->miniView()) {
                mini->setSpectrum(deEsserSpectrum.magnitudes(), deEsserSpectrum.numBins());
                if (effect != nullptr) {
                    mini->setDeEsser(*effect, liveRateHz);
                    // Sibilance-band level — the same signal the audio
                    // processor reduces. Broadband peak (levelDb, shared
                    // with the Multiband card) hid quiet sibilance under a
                    // loud 440 Hz voice, so the red bar sat at zero.
                    const float bandLevelDb = deEsserBandLevelDb(
                        tapLeft.data(), tapRight.data(), MiniSpectrum::kFftSize,
                        *effect, liveRateHz > 0.0 ? liveRateHz : 48000.0,
                        deEsserDetS1L, deEsserDetS2L, deEsserDetS1R, deEsserDetS2R);
                    deEsserReductionDb = std::abs(deEsserStaticGainDb(bandLevelDb, *effect));
                    mini->setDeEsserGainReduction(deEsserReductionDb);
                }
            }
        }
    }

    if (tap != nullptr && limiterCard != nullptr) {
        auto* useTap = limTap != nullptr ? limTap : tap;
        useTap->snapshot(false, tapLeft.data(), tapRight.data(), kLevelFrames);
        float peak = 0.0f;
        for (int i = 0; i < kLevelFrames; ++i)
            peak = std::max(peak, std::max(std::abs(tapLeft[static_cast<std::size_t>(i)]),
                                           std::abs(tapRight[static_cast<std::size_t>(i)])));
        const float levelDb = 20.0f * std::log10(std::max(peak, 1.0e-6f));
        const auto* effect = [&]() -> const pluginrack::LimiterEffect* {
            if (auto* channel = currentChannel()) {
                for (const auto& e : channel->limiterEffects)
                    if (!e.bypassed) return &e;
            } else if (auto* output = currentOutput()) {
                for (const auto& e : output->limiterEffects)
                    if (!e.bypassed) return &e;
            }
            return nullptr;
        }();
        if (auto* mini = limiterCard->miniView()) {
            if (effect != nullptr) {
                mini->setLimiter(*effect);
                // Brickwall gain: input above the ceiling is reduced by the
                // overshoot, the same maths the audio processor applies.
                const float gr = effect->bypassed
                                     ? 0.0f
                                     : -std::max(0.0f, levelDb - effect->ceilingDb);
                mini->setLimiterGain(levelDb, gr);
            }
        }
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

    // The incoming editor's native window is hidden until it is revealed, so
    // the plugin cannot raise a half-drawn window over the outgoing editor.

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
                                && !swappingEditor
                                && dockedEditor->isShowing();
    const auto previewMode = audioEngine.getRackPreviewMode();
    const int targetWidth = juce::jmax(32, slotViewport.getWidth() - 2 * kSlotInset);
    if (wantsThumbnail) {
        if (auto* pluginWindow = nativedisplay::pluginWindowOf(*dockedEditor)) {
            // The worker keeps only the newest request, so a slow plugin lowers
            // the thumbnail rate instead of backing the UI thread up.
            const auto insertId = focusedInsertId;
            juce::Component::SafePointer<RackView> safe(this);
            thumbnailWorker.request(pluginWindow, targetWidth,
                                    [safe, insertId](juce::Image thumbnail) {
                if (safe != nullptr) safe->applyVstThumbnail(insertId, thumbnail);
            });
        } else {
            // A software-drawn editor can only be rendered on the message thread, so
            // pace it with what the previous snapshot cost.
            const auto now = juce::Time::getMillisecondCounter();
            const auto interval = static_cast<juce::uint32>(
                juce::jmax(1000 / juce::jmax(1, fps), 4 * lastSnapshotCostMs));
            if (now - lastSnapshotMs >= interval) {
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
        }
    } else {
        thumbnailWorker.clearRequest();
    }

    // Background previews: one card per turn, at a quarter of the focused
    // rate, so the bay stays responsive while the rest catch up. Rotate stays
    // on the selected strip; AllLive also reaches the other strips, so their
    // cards are already current when you switch to them.
    if (previewMode == pluginrack::RackPreviewMode::Selected) {
        hiddenTick = 0;
        clearHiddenPreviews();
    } else if (++hiddenTick >= 4) {
        hiddenTick = 0;
        refreshHiddenPreviews(targetWidth, previewMode == pluginrack::RackPreviewMode::AllLive);
    }
}

void RackView::applyVstThumbnail(std::uint32_t insertId, const juce::Image& thumbnail) {
    if (thumbnail.isNull()) return;
    vstThumbs[insertId] = thumbnail;
    if (const auto card = vstCards.find(insertId); card != vstCards.end())
        if (auto* mini = card->second->miniView()) mini->setVstSnapshot(thumbnail);
}

namespace {
// How many background editors may exist at once. Each is a live plugin UI on
// its own thread, so this is a stability budget: a rack that keeps eight UIs
// alive froze the app, and the plugins here are heavy.
constexpr std::size_t kMaxHiddenPreviews = 4;
// Background preview editors are native windows parked at their unscaled size.
// Some plugins (Ozone, Ripeness, KERN CHECK) also create extra top-level
// windows that ignore the parking, so the previews leak over the app as white
// rectangles and duplicate plugin UIs. Keep them off until they can be hosted
// without native windows.
constexpr bool kEnableHiddenPreviews = false;
} // namespace

void RackView::refreshHiddenPreviews(int targetWidth, bool everyStrip) {
    if (!kEnableHiddenPreviews) {
        if (!hiddenPreviews.empty()) clearHiddenPreviews();
        return;
    }
    const auto* inserts = currentInserts();
    if (inserts == nullptr || inserts->empty()) {
        if (hiddenPreviews.empty()) return;
        clearHiddenPreviews();
        return;
    }

    // A hidden editor is owned by its insert's PluginHost. The engine destroys
    // those hosts on erase and scene restore, so retire anything that no
    // longer exists before touching a pointer.
    const auto stillExists = [this, inserts, everyStrip](std::uint32_t id) {
        const auto inStrip = std::any_of(inserts->begin(), inserts->end(),
                                         [id](const auto& insert) { return insert->id == id; });
        if (inStrip) return true;
        if (!everyStrip) return false;
        for (const auto& channel : audioEngine.getChannels())
            for (const auto& insert : channel->inserts)
                if (insert->id == id) return true;
        for (const auto& output : audioEngine.getOutputs())
            for (const auto& insert : output->inserts)
                if (insert->id == id) return true;
        return false;
    };
    for (auto it = hiddenPreviews.begin(); it != hiddenPreviews.end();) {
        if (stillExists(it->first)) {
            ++it;
        } else {
            retireHiddenPreview(it->first);
            it = hiddenPreviews.begin();
        }
    }

    // One capture per turn, round-robin over the eligible inserts.
    std::vector<std::pair<std::uint32_t, PluginHost*>> candidates;
    const auto consider = [&](AudioEngine::InsertSlot& insert) {
        if (insert.id == focusedInsertId) return;
        if (hiddenPreviewFailed.count(insert.id) != 0) return;
        candidates.emplace_back(insert.id, &insert.pluginHost);
    };
    for (const auto& insert : *inserts) consider(*insert);
    if (everyStrip) {
        for (const auto& channel : audioEngine.getChannels())
            for (const auto& insert : channel->inserts) consider(*insert);
        for (const auto& output : audioEngine.getOutputs())
            for (const auto& insert : output->inserts) consider(*insert);
    }
    if (candidates.empty()) return;

    hiddenCursor = (hiddenCursor + 1) % candidates.size();
    const auto [insertId, host] = candidates[hiddenCursor];

    auto& slot = hiddenPreviews[insertId];
    if (slot.editor == nullptr) {
        // The cap counts editors that already exist; this turn's new one has
        // to fit too, or a big rack would spawn one UI per insert.
        if (hiddenPreviews.size() > kMaxHiddenPreviews) {
            hiddenPreviews.erase(insertId);
            return;
        }
        slot.host = host;
        slot.editor = host->createEditorForDock();
        if (slot.editor == nullptr) {
            // No GUI, or the plugin refuses a second editor. Freeze the card
            // rather than retrying this insert on every turn.
            hiddenPreviewFailed.insert(insertId);
            hiddenPreviews.erase(insertId);
            return;
        }
        addAndMakeVisible(slot.editor);
        slot.editor->setVisible(true);
        // Read the plugin's own size after it has attached, then park it out
        // of sight at that size. A visible VST3 re-sizes itself here, so the
        // parked bounds must be applied after the visibility change.
        slot.naturalSize = slot.editor->getLocalBounds();
        if (slot.naturalSize.getWidth() < 32 || slot.naturalSize.getHeight() < 32)
            slot.naturalSize = { 0, 0, 640, 400 };
    }
    // A plugin is free to resize itself; keep it parked out of sight instead
    // of letting it paint over the app.
    // Park inside our own area, hidden behind the app window. A window placed
    // off-screen renders nothing to PrintWindow, which is why background
    // captures came back empty.
    const auto parked = juce::Rectangle<int>(0, 0,
                                             slot.naturalSize.getWidth(),
                                             slot.naturalSize.getHeight());
    if (slot.editor->getBounds() != parked)
        slot.editor->setBounds(parked);

    if (auto* pluginWindow = nativedisplay::pluginWindowOf(*slot.editor)) {
        // Keep the plugin's own window out of sight: JUCE makes it a child of
        // the app window, and a child always paints above its parent's
        // content. Re-parenting it into a window that sits *below* the app
        // leaves it invisible while it keeps rendering for the capture.
        if (previewHostWindow == nullptr) previewHostWindow = nativedisplay::createHiddenHost();
        nativedisplay::parkBehindApp(pluginWindow,
                                     getTopLevelComponent()->getWindowHandle(),
                                     previewHostWindow);
        juce::Component::SafePointer<RackView> safe(this);
        thumbnailWorker.request(pluginWindow, targetWidth,
                                [safe, insertId](juce::Image thumbnail) {
            if (safe == nullptr) return;
            if (thumbnail.isNull()) {
                // Report once per insert: an empty capture is why a card would
                // keep showing "open to capture preview".
                if (safe->emptyCaptureLogged.insert(insertId).second)
                    juce::Logger::writeToLog("rack: preview capture empty for insert "
                                             + juce::String(insertId));
                return;
            }
            safe->applyVstThumbnail(insertId, thumbnail);
        });
    } else {
        const auto bounds = slot.editor->getLocalBounds();
        if (!bounds.isEmpty()) {
            const float scale = juce::jlimit(0.02f, 1.0f,
                                             static_cast<float>(targetWidth)
                                                 / static_cast<float>(bounds.getWidth()));
            applyVstThumbnail(insertId,
                              slot.editor->createComponentSnapshot(bounds, true, scale));
        }
    }
}

void RackView::retireHiddenPreview(std::uint32_t insertId) {
    const auto it = hiddenPreviews.find(insertId);
    if (it == hiddenPreviews.end()) return;
    auto* editor = it->second.editor;
    auto* host = it->second.host;
    hiddenPreviews.erase(it);
    hiddenPreviewFailed.erase(insertId);
    if (editor == nullptr) return;
    // Stop new captures of this window, then delete it once the worker is
    // idle. Blocking here would deadlock: PrintWindow asks this thread to
    // paint.
    thumbnailWorker.clearRequest();
    doomedPreviews[insertId] = { editor, host };
    destroyEditorWhenIdle(editor, host, insertId);
}

void RackView::clearHiddenPreviews() {
    hiddenPreviewFailed.clear();
    if (hiddenPreviews.empty()) return;
    thumbnailWorker.clearRequest();
    for (auto& [id, slot] : hiddenPreviews) {
        doomedPreviews[id] = { slot.editor, slot.host };
        destroyEditorWhenIdle(slot.editor, slot.host, id);
    }
    hiddenPreviews.clear();
}

void RackView::releaseEditorsNow() {
    // Must run while every processor is still alive: a plugin editor's
    // destructor calls back into its processor, so deleting one after the
    // engine destroyed the insert is a use-after-free.
    knobWatcher.clear();
    thumbnailWorker.clearRequest();

    const auto destroyEditor = [this](juce::AudioProcessorEditor* editor, PluginHost* host) {
        if (editor == nullptr) return;
        if (auto* parent = editor->getParentComponent())
            parent->removeChildComponent(editor);
        if (host != nullptr && pluginHostAlive(host)) {
            host->destroyDockEditor(editor);
        } else {
            // Already orphaned by a path that slipped past this one: deleting
            // it would call into freed plugin state, so leak it instead.
            juce::Logger::writeToLog(
                "rack: editor released after its processor; leaking it");
        }
    };

    for (auto& entry : hiddenPreviews) destroyEditor(entry.second.editor, entry.second.host);
    hiddenPreviews.clear();
    hiddenPreviewFailed.clear();

    for (auto& entry : doomedPreviews) destroyEditor(entry.second.editor, entry.second.host);
    doomedPreviews.clear();

    if (dockedEditor == nullptr) {
        dockedHost = nullptr;
        return;
    }
    // A floating window may own the editor after a reparent; ~PluginHost
    // deletes that window before the processor, so release the pointer only.
    if (dockedEditor->getParentComponent() != nullptr
        && dockedEditor->getParentComponent() != &editorHolder) {
        dockedEditor = nullptr;
        dockedHost = nullptr;
        dockedEditorNaturalSize = {};
        dockedEditorScale = 1.0f;
        applyingDockedEditorScale = false;
        return;
    }
    dockedEditor->removeComponentListener(this);
    auto* editor = dockedEditor;
    auto* host = dockedHost;
    dockedEditor = nullptr;
    dockedHost = nullptr;
    dockedEditorNaturalSize = {};
    dockedEditorScale = 1.0f;
    applyingDockedEditorScale = false;
    destroyEditor(editor, host);
}

void RackView::destroyEditorWhenIdle(juce::AudioProcessorEditor* editor, PluginHost* host,
                                     std::uint32_t insertId) {
    if (editor == nullptr) return;
    juce::Component::SafePointer<juce::AudioProcessorEditor> safe(editor);
    juce::Component::SafePointer<RackView> self(this);
    juce::Timer::callAfterDelay(80, [self, safe, host, insertId] {
        if (self == nullptr || safe == nullptr) return;
        if (self->thumbnailWorker.isCapturing()) {
            // Still printing this window; try again on a later turn.
            self->destroyEditorWhenIdle(safe.getComponent(), host, insertId);
            return;
        }
        auto* component = safe.getComponent();
        self->removeChildComponent(component);
        if (host == nullptr) {
            delete component;
        } else if (self->pluginHostAlive(host)) {
            host->destroyDockEditor(component);
        } else {
            // The processor is gone; deleting the editor now calls into freed
            // plugin state. releaseEditorsNow() should have handled this before
            // the strip died, so leak rather than crash if one slipped through.
            juce::Logger::writeToLog(
                "rack: deferred editor outlived its processor; leaking it");
        }
        if (insertId != 0 && self->doomedPreviews.erase(insertId) != 0) {
            // The processor is free again: the bay can dock its editor now.
            self->refreshEditor();
        }
    });
}

void RackView::refreshEditor() {
    // Any refresh means the bay is being reassigned. A built-in panel left
    // docked here would otherwise stay visible above the incoming VST editor
    // (or keep the bay from showing it at all).
    if (focusedEffectWindow != nullptr)
        closeEffectWindows();

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
    // JUCE hands out one editor per plugin: while this insert's preview editor
    // is still being deleted, asking for another would fail with "no editor".
    // destroyEditorWhenIdle() calls back here once the delete is done.
    if (focus != nullptr && doomedPreviews.count(focus->id) != 0) {
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
        return;
    }
    if (focus == nullptr) {
        closeDockedEditor();
        swappingEditor = false;
        editorWindowShown = false;
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
    // Keep the current editor on screen while its replacement is created and
    // laid out. Removing it first leaves the bay empty for the whole time the
    // message thread spends building the new editor (200-400 ms on some
    // plugins), which the user sees as a flash.
    auto* previousEditor = dockedEditor;
    auto* previousHost = dockedHost;
    double swapPhaseStart = juce::Time::getMillisecondCounterHiRes();
    if (previousEditor != nullptr) {
        previousEditor->removeComponentListener(this);
        // Freeze what is on screen right now. Both native editors are hidden
        // for the rest of the swap, so this still frame is what keeps the bay
        // looking unchanged instead of flashing black or showing two UIs.
        if (auto* peer = getPeer()) {
            const auto bay = editorHolder.getScreenBounds();
            const auto scale = peer->getPlatformScaleFactor();
            auto* mainWindow = peer->getNativeHandle();
            const auto windowPixels = nativedisplay::windowRect(mainWindow);
            // Stray natural-size plugin windows can be parked just above the
            // app during a switch. A screen grab would bake them into the
            // freeze frame, so clear them before capturing.
            nativedisplay::hideOffscreenJuceChildren(peer->getNativeHandle(), nullptr);
            // Capture the window as it appears on screen. PrintWindow renders
            // JUCE's own child components black (they do not answer WM_PRINT),
            // so it cannot be used for a whole-window freeze. The app is the
            // active window here (the user just clicked an insert), so a screen
            // copy is complete and correctly aligned.
            auto whole = nativedisplay::captureScreenRegion(
                windowPixels.getX(), windowPixels.getY(), windowPixels.getWidth(),
                windowPixels.getHeight());
            if (whole.isNull())
                whole = nativedisplay::captureWindowForOverlay(mainWindow);
            if (!whole.isNull()) {
                const auto bayPixels = juce::Rectangle<int>(
                    juce::roundToInt(bay.getX() * scale), juce::roundToInt(bay.getY() * scale),
                    juce::roundToInt(bay.getWidth() * scale),
                    juce::roundToInt(bay.getHeight() * scale)).translated(-windowPixels.getX(),
                                                                      -windowPixels.getY());
                frozenBayImage = whole.getClippedImage(
                    bayPixels.getIntersection(whole.getBounds()));
            }
            if (frozenBayImage.isNull())
                frozenBayImage = nativedisplay::captureScreenRegion(
                    juce::roundToInt(bay.getX() * scale), juce::roundToInt(bay.getY() * scale),
                    juce::roundToInt(bay.getWidth() * scale),
                    juce::roundToInt(bay.getHeight() * scale));
            frozenBayActive = !frozenBayImage.isNull();
            if (frozenBayActive) {
                // Put the still frame in its own always-on-top native window.
                // JUCE paints below plugin windows, so a component-level image
                // would be covered by whatever native window the plugin shows
                // during creation; this window is guaranteed to be above them.
                frozenOverlay = std::make_unique<juce::ImageComponent>();
                // Freeze the whole window, not just the bay: a plugin's native
                // window can be taller than its fitted editor and paint above
                // the bay, over the side-chain/top bar. Covering the full app
                // area hides every stray window until the swap completes.
                const bool wholeWindow = !whole.isNull() && windowPixels.getWidth() > 0;
                frozenOverlay->setImage(wholeWindow ? whole : frozenBayImage);
                frozenOverlay->setImagePlacement(juce::RectanglePlacement::stretchToFit);
                frozenOverlay->setInterceptsMouseClicks(false, false);
                frozenOverlay->setOpaque(true);
                frozenOverlay->setAlwaysOnTop(true);
                frozenOverlay->addToDesktop(juce::ComponentPeer::windowIgnoresMouseClicks
                                            | juce::ComponentPeer::windowIsTemporary);
                if (wholeWindow)
                    frozenOverlay->setBounds(juce::Rectangle<int>(
                        juce::roundToInt(windowPixels.getX() / scale),
                        juce::roundToInt(windowPixels.getY() / scale),
                        juce::roundToInt(windowPixels.getWidth() / scale),
                        juce::roundToInt(windowPixels.getHeight() / scale)));
                else
                    frozenOverlay->setBounds(editorHolder.getScreenBounds());
                frozenOverlay->setVisible(true);
                frozenOverlay->toFront(false);
                if (auto* overlayPeer = frozenOverlay->getPeer()) {
                    overlayPeer->performAnyPendingRepaintsNow();
                    // The still frame covers the window from now on, so hide
                    // every plugin window underneath it. Some plugins (Ozone,
                    // Ripeness) show their UI through extra top-level windows
                    // that the ordinary hide calls never reach; they would
                    // otherwise stay visible beside the incoming editor.
                    nativedisplay::hideJuceWindowsInRect(peer->getNativeHandle(), nullptr,
                                                         overlayPeer->getNativeHandle(),
                                                         windowPixels.getX(), windowPixels.getY(),
                                                         windowPixels.getRight(),
                                                         windowPixels.getBottom());
                    nativedisplay::hideOtherProcessWindows(peer->getNativeHandle(), nullptr,
                                                           overlayPeer->getNativeHandle());
                }
            }
        }
    }
    if (previousEditor != nullptr) {
        swapStartedAt = swapPhaseStart;
        juce::Logger::writeToLog(
            "rack: swap freeze "
            + juce::String(juce::Time::getMillisecondCounterHiRes() - swapPhaseStart, 1)
            + " ms active=" + juce::String(frozenBayActive ? 1 : 0));
    }
    dockedEditor = nullptr;
    dockedHost = nullptr;
    dockedEditorNaturalSize = {};
    dockedEditorScale = 1.0f;
    applyingDockedEditorScale = false;
    dockedEditorLayoutPasses = 0;
    knobWatcher.clear();
    thumbnailWorker.clearRequest();
    // The old editor belongs to a different plugin instance, so it may stay
    // docked during the build. An editor a floating window reparented away is
    // owned by that window: never delete it here.
    const auto retirePrevious = [this, previousEditor, previousHost] {
        if (previousEditor == nullptr) return;
        if (auto* parent = previousEditor->getParentComponent()) {
            if (parent != &editorHolder) return;
            parent->removeChildComponent(previousEditor);
        }
        destroyEditorWhenIdle(previousEditor, previousHost);
    };

    dockedHost = &focus->pluginHost;
    dockedHost->closeFloatingEditor();
    dockedEditor = dockedHost->createEditorForDock();
    if (dockedEditor == nullptr) {
        dockedHost = nullptr;
        swappingEditor = false;
        editorWindowShown = false;
        retirePrevious();
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
    juce::Logger::writeToLog("rack: natural "
                             + juce::String(dockedEditorNaturalSize.getWidth()) + "x"
                             + juce::String(dockedEditorNaturalSize.getHeight()));

    noEditorLabel.setVisible(false);
    noEditorBypass.setVisible(false);
    const auto dockStart = juce::Time::getMillisecondCounterHiRes();
    auto dockElapsed = [dockStart] {
        return juce::String(juce::Time::getMillisecondCounterHiRes() - dockStart, 1);
    };
    dockedEditorLayoutPasses = 0;
    swappingEditor = previousEditor != nullptr;
    swapOutgoing = previousEditor;
    juce::Logger::writeToLog(
        "rack: swap create "
        + juce::String(juce::Time::getMillisecondCounterHiRes() - swapStartedAt, 1) + " ms");
    editorWindowShown = false;
    offscreenGuardUntilMs = juce::Time::getMillisecondCounterHiRes() + 2500.0;
    dockedEditor->addComponentListener(this);
    // createEditorAndMakeActive() may leave the editor visible already. Taking
    // it out of the hierarchy's paint path before adding it is what keeps its
    // native window from being created over the outgoing editor during the
    // blocking layout below.
    if (swappingEditor)
        dockedEditor->setVisible(false);
    editorHolder.addChildComponent(dockedEditor);
    // A plugin's view is a separate HWND, and a newly created VST3 view paints
    // white at its initial (unscaled) size until it has handled the fit. When
    // an editor is being swapped in, keep the component itself invisible while
    // the layout runs: making it visible creates and shows its native window,
    // and the layout pass below blocks the message thread long enough for that
    // window (at its natural size, on top) to be composited over the outgoing
    // editor. The window is created afterwards, already parked outside the
    // holder, and only then is the fit applied to a visible editor.
    if (swappingEditor)
        // Park far above the window: the VST3 view creates its native window at
        // the plugin's natural size, which is taller than the fitted component,
        // so parking by the component height still left its bottom edge inside
        // the app flashing over the top bar.
        dockedEditor->setTopLeftPosition(0, -4000);
    else
        dockedEditor->setVisible(true);
    if (previousEditor != nullptr && previousEditor->getParentComponent() == &editorHolder) {
        previousEditor->toFront(false);
        if (auto* window = nativedisplay::pluginWindowOf(*previousEditor))
            nativedisplay::raiseWindow(window);
    }
    knobWatcher.setPlugin(pluginInstance);
    // Hide the incoming view before the blocking layout runs, not just after:
    // the host is created as soon as the component is parented, and any frame
    // it composites from here until `editorWindowShown` must stay off screen.
    if (swappingEditor && !editorWindowShown)
        if (auto* window = nativedisplay::pluginWindowOf(*dockedEditor))
            nativedisplay::hideWindow(window);
    // A selection swap does not change any geometry outside the bay. Running
    // the full resized() here re-lays the whole rack (100-250 ms on some
    // sessions); only the insert cards' selection state and the docked editor
    // need touching.
    if (swappingEditor) {
        layoutSlots();
        layoutDockedEditor();
    } else {
        resized();
    }
    if (swappingEditor && !editorWindowShown) {
        dockedEditor->setVisible(true);
        if (auto* window = nativedisplay::pluginWindowOf(*dockedEditor))
            nativedisplay::hideWindow(window);
        // Apply the scale now that the editor is visible but still off-screen:
        // the first layout ran with the editor hidden and did not touch it, so
        // the plugin never created a view over the outgoing editor.
        applyDockedEditorFit(editorHolder.getLocalBounds());
        layoutDockedEditor();
    }
    if (swappingEditor) {
        juce::Logger::writeToLog(
            "rack: swap layout "
            + juce::String(juce::Time::getMillisecondCounterHiRes() - swapStartedAt, 1) + " ms");
    }
    // Keep the outgoing editor on screen until the incoming one has been
    // fitted and has painted. The incoming window is hidden meanwhile, so its
    // unscaled white/black frames cannot show at all. Once the editor reports
    // its fitted size for two consecutive ticks (or the cap expires), hide the
    // old window and show the new one in the same message turn: only one
    // plugin is ever visible.
    if (previousEditor != nullptr && previousEditor->getParentComponent() == &editorHolder) {
        struct SwapState { int ticks = 0; int total = 0; int shownTicks = 0; int captures = 0;
                           bool shown = false; bool capturePending = false; };
        auto state = std::make_shared<SwapState>();
        auto poll = std::make_shared<std::function<void()>>();
        auto finish = std::make_shared<std::function<void()>>();
        juce::Component::SafePointer<RackView> safeSwap(this);
        auto* incoming = dockedEditor;
        auto* outgoing = previousEditor;
        auto retire = retirePrevious;
        auto* holder = &editorHolder;
        *finish = [safeSwap, incoming, outgoing, retire, state]() {
            auto* self = safeSwap.getComponent();
            if (self == nullptr) return;
            if (self->dockedEditor != incoming || outgoing->getParentComponent() != &self->editorHolder) {
                retire();
                return;
            }
            juce::Logger::writeToLog(
                "rack: swap done "
                + juce::String(juce::Time::getMillisecondCounterHiRes() - self->swapStartedAt, 1)
                + " ms ticks=" + juce::String(state->shownTicks)
                + " captures=" + juce::String(state->captures));
            self->swappingEditor = false;
            self->editorWindowShown = false;
            self->swapOutgoing = nullptr;
            self->swapGuardUntilMs = juce::Time::getMillisecondCounterHiRes() + 3000.0;
            // The incoming editor is already in the bay and has painted behind
            // the outgoing one. Reveal it in the same message turn as the old
            // one is hidden, so no blank frame is composited in between.
            self->layoutDockedEditor();
            // Hide every other native window in the bay (including the
            // outgoing editor and any copy the plugin created behind our back)
            // before revealing the incoming one, so exactly one UI is visible.
            auto* incomingWindow = nativedisplay::pluginWindowOf(*incoming);
            if (auto* peer = self->getPeer()) {
                const auto windowPixels = nativedisplay::windowRect(peer->getNativeHandle());
                void* overlayWindow = nullptr;
                if (self->frozenOverlay != nullptr)
                    if (auto* overlayPeer = self->frozenOverlay->getPeer())
                        overlayWindow = overlayPeer->getNativeHandle();
                nativedisplay::hideJuceWindowsInRect(peer->getNativeHandle(), incomingWindow,
                                                     overlayWindow, windowPixels.getX(),
                                                     windowPixels.getY(), windowPixels.getRight(),
                                                     windowPixels.getBottom());
            }
            if (auto* peer = self->getPeer()) {
                void* overlayWindow = nullptr;
                if (self->frozenOverlay != nullptr)
                    if (auto* overlayPeer = self->frozenOverlay->getPeer())
                        overlayWindow = overlayPeer->getNativeHandle();
                // Hide any top-level window the outgoing editor left at the
                // screen origin; those are the copies that cover the left side
                // during a switch.
                nativedisplay::hideOtherProcessWindows(peer->getNativeHandle(), incomingWindow,
                                                       overlayWindow);
            }
            if (auto* editorPeer = incoming->getPeer())
                if (editorPeer != self->editorHolder.getPeer())
                    nativedisplay::showWindowNoActivate(editorPeer->getNativeHandle());
            if (incomingWindow != nullptr) {
                nativedisplay::clearWindowClip(incomingWindow);
                incoming->toFront(false);
                nativedisplay::showWindowNoActivate(incomingWindow);
                nativedisplay::raiseWindow(incomingWindow);
                // Some plugins (Ripeness) create their root at the natural size
                // and do not follow a later host resize, leaving their UI in the
                // top-left of a white window. Bounce the bounds once while the
                // editor is visible so the wrapper sends a fresh onSize.
                const auto revealed = incoming->getBounds();
                incoming->setBounds(revealed.withWidth(revealed.getWidth() + 1));
                incoming->setBounds(revealed);
                nativedisplay::repaintWindowNow(incomingWindow);
            }
            if (auto* outgoingWindow = nativedisplay::pluginWindowOf(*outgoing))
                nativedisplay::hideWindow(outgoingWindow);
            if (self->frozenBayActive) {
                // The incoming UI is on screen now; drop the still frame in the
                // same turn so there is no blank gap between the two.
                self->frozenOverlay.reset();
                self->frozenBayActive = false;
                self->frozenBayImage = {};
                self->repaint(self->editorHolder.getBounds());
            }
            // The incoming plugin can park a natural-size window above the app
            // while it re-renders after the reveal; that window is what shows
            // as a white bar over the top bar.
            self->hideOffscreenPluginWindows();
            retire();
        };
        *poll = [safeSwap, incoming, outgoing, retire, state, poll, finish, holder]() mutable {
            auto* self = safeSwap.getComponent();
            if (self == nullptr) return;
            if (self->dockedEditor != incoming || outgoing->getParentComponent() != holder) {
                // Superseded by another selection: the newer swap owns the
                // incoming editor, so only retire the one this swap owned.
                retire();
                return;
            }
            auto* incomingWindow = nativedisplay::pluginWindowOf(*incoming);
            auto* outgoingWindow = nativedisplay::pluginWindowOf(*outgoing);
            // The editor may own a top-level peer of its own (the unscaled
            // window that paints white). It is created lazily after the first
            // layout pass, so re-apply the mask every tick, not only around
            // the fit.
            self->maskIncomingWindow();
            if (state->shown) {
                // The new window is on screen behind the old one. It only
                // produces its first frame when it is actually visible, so
                // wait for the capture worker to see real pixels, then flip.
                if (!self->frozenBayActive && outgoingWindow != nullptr && incomingWindow != nullptr) {
                    nativedisplay::placeWindowBelow(incomingWindow, outgoingWindow);
                    // The incoming host can still be at its natural size here;
                    // keep its region inside the outgoing editor so its
                    // unscaled overflow (white band) cannot show around it.
                    nativedisplay::clipWindowToWindow(incomingWindow, outgoingWindow);
                    nativedisplay::raiseWindow(outgoingWindow);
                    outgoing->toFront(false);
                }
                ++state->shownTicks;
                // PrintWindow is the only reliable paint signal for a VST3
                // view: GPU views never answer WM_PAINT. The worker runs it
                // off the message thread and the callback arrives async. Give
                // it a few ticks first so an all-background frame is not taken
                // for a finished one.
                // While the frozen frame covers the whole window, the swap is
                // invisible; a fixed short settle is enough for the incoming
                // view to produce its first frame. PrintWindow on a hidden or
                // clipped view can take hundreds of ms and then time out, which
                // is what made the switch feel slow.
                const int captureFrom = 6;
                if (!self->frozenBayActive && !state->capturePending && state->captures < 12
                    && state->shownTicks >= captureFrom && incomingWindow != nullptr) {
                    ++state->captures;
                    state->capturePending = true;
                    auto finishCopy = finish;
                    self->thumbnailWorker.request(incomingWindow, 16,
                        [state, finishCopy](juce::Image captured) mutable {
                            state->capturePending = false;
                            if (captured.isValid() && !captured.isNull())
                                (*finishCopy)();
                        });
                }
                if (state->shownTicks >= (self->frozenBayActive ? 4 : 20)) {
                    (*finish)();
                    return;
                }
                juce::Timer::callAfterDelay(16, [poll] { (*poll)(); });
                return;
            }
            // JUCE/the plugin can re-show the view during a resize; keep it
            // hidden until it is fitted, so it cannot peek over the old editor.
            if (incomingWindow != nullptr) {
                nativedisplay::hideWindow(incomingWindow);
                if (outgoingWindow != nullptr) {
                    // The host can be recreated by the plugin while it is being
                    // fitted; re-apply the mask every tick, not just after the
                    // fit, so a fresh unscaled window cannot show its white
                    // overflow around the outgoing editor.
                    nativedisplay::placeWindowBelow(incomingWindow, outgoingWindow);
                    nativedisplay::clipWindowToWindow(incomingWindow, outgoingWindow);
                    nativedisplay::raiseWindow(outgoingWindow);
                }
            }
            const auto visual = self->dockedEditorVisualBounds();
            const auto bay = holder->getLocalBounds();
            const bool fitted = visual.getWidth() > 0 && visual.getHeight() > 0
                && visual.getWidth() <= bay.getWidth() + 4
                && visual.getHeight() <= bay.getHeight() + 4;
            state->ticks = fitted ? state->ticks + 1 : 0;
            if (state->ticks >= 2 || ++state->total >= 25) {
                // The incoming editor has an accepted size now. Move it into the
                // bay while its native window is still hidden, then show it
                // directly below the old one: the old editor stays the only
                // visible UI while the new one renders its first frame.
                juce::Logger::writeToLog(
                    "rack: swap fitted at "
                    + juce::String(juce::Time::getMillisecondCounterHiRes() - self->swapStartedAt, 1)
                    + " ms ticks=" + juce::String(state->total));
                if (incomingWindow != nullptr) {
                    self->editorWindowShown = true;
                    self->layoutDockedEditor();
                    if (self->frozenBayActive) {
                        // The overlay window covers the bay, so let the incoming
                        // window show and paint underneath it. finish() removes
                        // the overlay in the same turn the old editor is hidden.
                        nativedisplay::showWindowNoActivate(incomingWindow);
                    } else {
                        nativedisplay::hideWindow(incomingWindow);
                        if (outgoingWindow != nullptr)
                            nativedisplay::placeWindowBelow(incomingWindow, outgoingWindow);
                        nativedisplay::showWindowNoActivate(incomingWindow);
                        if (outgoingWindow != nullptr) {
                            nativedisplay::placeWindowBelow(incomingWindow, outgoingWindow);
                            nativedisplay::clipWindowToWindow(incomingWindow, outgoingWindow);
                            nativedisplay::raiseWindow(outgoingWindow);
                        }
                    }
                }
                // The layout passes must not hide it or move it out of the bay
                // again; the swap flag stays set so the thumbnail timer does not
                // replace the readiness capture.
                self->editorWindowShown = true;
                if (!self->frozenBayActive && outgoingWindow != nullptr) {
                    nativedisplay::raiseWindow(outgoingWindow);
                    outgoing->toFront(false);
                }
                state->shown = true;
                juce::Timer::callAfterDelay(16, [poll] { (*poll)(); });
                return;
            }
            juce::Timer::callAfterDelay(16, [poll] { (*poll)(); });
        };
        juce::Timer::callAfterDelay(16, [poll] { (*poll)(); });
    } else {
        // Nothing to mask (first editor, or the old one was floating). The
        // incoming window was never hidden in this case.
        swappingEditor = false;
        editorWindowShown = false;
        if (auto* window = nativedisplay::pluginWindowOf(*dockedEditor)) {
            nativedisplay::raiseWindow(window);
            nativedisplay::repaintWindowNow(window);
        }
        retirePrevious();
    }
    // Some plugins create their own native window at their preferred size
    // and put it back after it has been placed. Re-apply the bay fit once
    // the plugin has settled instead of fighting it in a resize loop.
    //
    // The callbacks run on the message thread and are the only nest a
    // plugin-initiated resize can still land in: layoutDockedEditor() owns
    // its own guard, and the bay is only resized by resized() here.
    juce::Component::SafePointer<RackView> safe(this);
    for (const int delayMs : {400, 1200}) {
        juce::Timer::callAfterDelay(delayMs, [safe] {
            if (safe == nullptr) return;
            safe->dockedEditorLayoutPasses = 0;
            safe->layoutDockedEditor();
            safe->hideOffscreenPluginWindows();
        });
    }
    // Docking a native plugin UI is the slow part of adding one (some editors
    // take ~1-2.5 s to attach), so it is reported in the run log.
    juce::Logger::writeToLog("rack: editor docked " + dockElapsed() + " ms");
}

void RackView::updateSideChainBar() {
    // Built-in panels (EQ, dynamics, multiband, ...) fill the bay themselves;
    // the strip belongs to a docked plugin editor only.
    auto* focus = focusedInsertId != 0 ? findInsert(focusedInsertId) : nullptr;
    const bool stripExists = currentChannel() != nullptr || currentOutput() != nullptr;
    const bool show = focus != nullptr && stripExists && !eqFocused && !dynamicsFocused
                      && focusedEffectWindow == nullptr;
    sideChainBarVisible = show;
    sideChainBar.setVisible(show);
    if (!show) return;
    const auto ownerKind = !selectedIsOutput ? SideChainBar::OwnerKind::Channel
                          : isSelectedBus()   ? SideChainBar::OwnerKind::Bus
                                              : SideChainBar::OwnerKind::Output;
    sideChainBar.setTarget(focus, ownerKind, static_cast<std::uint32_t>(selectedId));
}

void RackView::maskIncomingWindow() {
    if (!swappingEditor || swapOutgoing == nullptr || dockedEditor == nullptr)
        return;

    hideOffscreenPluginWindows();

    if (frozenBayActive) {
        // The bay is covered by the overlay window, so nothing under it can be
        // seen. Keep the incoming editor (once it exists) so it can paint, and
        // hide every other plugin window in the bay, including the top-level
        // copies that pluginWindowOf() does not return.
        hideOffscreenPluginWindows();
        if (auto* peer = getPeer()) {
            auto* incoming = editorWindowShown ? nativedisplay::pluginWindowOf(*dockedEditor) : nullptr;
            void* overlayWindow = nullptr;
            if (frozenOverlay != nullptr)
                if (auto* overlayPeer2 = frozenOverlay->getPeer())
                    overlayWindow = overlayPeer2->getNativeHandle();
            const auto windowPixels = nativedisplay::windowRect(peer->getNativeHandle());
            nativedisplay::hideJuceWindowsInRect(peer->getNativeHandle(), incoming, overlayWindow,
                                                 windowPixels.getX(), windowPixels.getY(),
                                                 windowPixels.getRight(),
                                                 windowPixels.getBottom());
            // The incoming editor may also open an own top-level window at the
            // screen origin while it is created; keep every such stray hidden
            // while the frozen frame is on screen.
            nativedisplay::hideOtherProcessWindows(peer->getNativeHandle(), incoming, overlayWindow);
        }
        return;
    }

    // Some plugins (Ozone, Ripeness) expose their UI through a second native
    // window that pluginWindowOf() never returns. Those windows ignore every
    // hide/raise below, which is what leaves two plugins visible at once and
    // the white bar over the top bar. Hide everything in the bay except the
    // editors we intend to show: the outgoing one always, plus the incoming
    // one once it is parked behind it.
    if (auto* peer = getPeer()) {
        auto* outgoing = nativedisplay::pluginWindowOf(*swapOutgoing);
        auto* incoming = editorWindowShown ? nativedisplay::pluginWindowOf(*dockedEditor) : nullptr;
        const auto bay = editorHolder.getScreenBounds();
        nativedisplay::hideJuceWindowsInRect(peer->getNativeHandle(), outgoing, incoming,
                                             bay.getX(), bay.getY(),
                                             bay.getRight(), bay.getBottom());
    }

    if (editorWindowShown) return;
    auto* incoming = nativedisplay::pluginWindowOf(*dockedEditor);
    auto* outgoing = nativedisplay::pluginWindowOf(*swapOutgoing);
    if (incoming == nullptr || outgoing == nullptr) return;
    // Keep only the part of the incoming native window that overlaps the
    // editor it replaces. An unscaled VST3 view is wider/taller than the bay,
    // and its white unpainted overflow otherwise shows over the output column.
    nativedisplay::placeWindowBelow(incoming, outgoing);
    nativedisplay::clipWindowToWindow(incoming, outgoing);
    nativedisplay::raiseWindow(outgoing);
}

void RackView::hideOffscreenPluginWindows() {
    if (auto* peer = getPeer())
        nativedisplay::hideOffscreenJuceChildren(peer->getNativeHandle(), nullptr);
}

void RackView::layoutDockedEditor() {
    if (dockedEditor == nullptr || layingOutDockedEditor) return;
    const juce::ScopedValueSetter<bool> guard(layingOutDockedEditor, true);
    const auto bay = editorHolder.getLocalBounds();
    if (bay.isEmpty()) return;

    // Hide before the fit and the bounds write: creating or resizing the
    // native window can take a whole frame, and an unpainted window on top of
    // the outgoing editor is exactly the black flash being removed.
    if (swappingEditor && !editorWindowShown)
        if (auto* window = nativedisplay::pluginWindowOf(*dockedEditor))
            nativedisplay::hideWindow(window);

    // Applying the content scale makes a VST3 wrapper create and show its view.
    // While an editor is being swapped in and has not been made visible yet,
    // leave the scale alone: the first layout after this one applies it while
    // the new window is already parked outside the bay and hidden.
    if (!swappingEditor || editorWindowShown)
        applyDockedEditorFit(bay);

    // What the editor paints now: a VST3 re-sizes itself when its content scale
    // changes, while a software-drawn editor keeps its own size and is scaled
    // by its transform instead. Either way the result is centred in the bay and
    // never leaves it.
    const auto visual = dockedEditorVisualBounds();
    const auto fitted = bay.withSizeKeepingCentre(
        juce::jmax(1, juce::jmin(visual.getWidth(), bay.getWidth())),
        juce::jmax(1, juce::jmin(visual.getHeight(), bay.getHeight())));

    auto placement = fitted;
    // While an editor is first being built, keep its native window parked
    // outside the bay: it is created during the bounds write below and may show
    // itself before we can hide it again. Once it has been shown (behind the
    // outgoing editor) it moves into the bay and stays there, so the frame it
    // paints while covered is the one the reveal shows.
    if (swappingEditor && !editorWindowShown)
        placement.setPosition(0, -4000);

    if (!dockedEditor->getTransform().isIdentity()) {
        // The transform scales away from the component's top-left, so the
        // position is what puts the scaled drawing in the middle.
        if (dockedEditor->getPosition() != placement.getPosition())
            dockedEditor->setTopLeftPosition(placement.getPosition());
    } else if (dockedEditor->getBounds() != placement) {
        // Writing sizes never deadlocks on its own: the fit itself is
        // synchronous here, and the pass cap below stops the layout from
        // re-entering through componentMovedOrResized().
        dockedEditor->setBounds(placement);
        juce::Logger::writeToLog("rack: editor placed " + placement.toString() + " in bay "
                                 + bay.toString());
    }
    // The plugin's native window is created when the editor first gets its
    // bounds, i.e. inside this call. Hide it straight away while a swap is in
    // progress, before this message turn can paint its unpainted surface over
    // the outgoing editor.
    if (swappingEditor && !editorWindowShown)
        if (auto* window = nativedisplay::pluginWindowOf(*dockedEditor))
            nativedisplay::hideWindow(window);

    // A plugin re-shows its view when its content scale changes. While a swap
    // is in progress, put the incoming window straight back below the outgoing
    // editor so that re-show cannot expose an unpainted frame.
    if (swappingEditor && editorWindowShown && swapOutgoing != nullptr) {
        if (auto* incoming = nativedisplay::pluginWindowOf(*dockedEditor))
            if (auto* outgoing = nativedisplay::pluginWindowOf(*swapOutgoing)) {
                nativedisplay::placeWindowBelow(incoming, outgoing);
                nativedisplay::raiseWindow(outgoing);
            }
    }
    maskIncomingWindow();
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

    if (swappingEditor && !editorWindowShown)
        if (auto* window = nativedisplay::pluginWindowOf(*dockedEditor))
            nativedisplay::hideWindow(window);

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
    juce::Logger::writeToLog("rack: fit " + juce::String(fit, 3) + " natural "
                             + juce::String(naturalWidth) + "x" + juce::String(naturalHeight)
                             + " before " + before.toString() + " after " + after.toString()
                             + " rescaled=" + juce::String(rescaled ? 1 : 0));
    // A VST3 wrapper may show the view when its content scale changes. Keep it
    // hidden while this editor is still the incoming one in a swap.
    if (swappingEditor && !editorWindowShown)
        if (auto* window = nativedisplay::pluginWindowOf(*dockedEditor))
            nativedisplay::hideWindow(window);

    // A plugin re-shows its view when its content scale changes. While a swap
    // is in progress, put the incoming window straight back below the outgoing
    // editor so that re-show cannot expose an unpainted frame.
    if (swappingEditor && editorWindowShown && swapOutgoing != nullptr) {
        if (auto* incoming = nativedisplay::pluginWindowOf(*dockedEditor))
            if (auto* outgoing = nativedisplay::pluginWindowOf(*swapOutgoing)) {
                nativedisplay::placeWindowBelow(incoming, outgoing);
                nativedisplay::raiseWindow(outgoing);
            }
    }
    maskIncomingWindow();
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
    // Gated on a changed size: a move-only notification that outlived the
    // layout that triggered it must not reach a plugin that already called
    // closeDockedEditor() from its own paint, and a message-thread timer
    // pumping layout must never be re-entered from the render path.
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
    // that keeps insisting on a size the bay cannot hold. A plugin parked in
    // a layout loop still stops after the cap: no resize reaches render past
    // this point, so nothing here can hold the message thread.
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
    swappingEditor = false;
    editorWindowShown = false;
    swapOutgoing = nullptr;
    frozenOverlay.reset();
    frozenBayActive = false;
    frozenBayImage = {};
    if (dockedEditor == nullptr) {
        dockedHost = nullptr;
        return;
    }
    // A capture may be printing this editor's window. Never block on it:
    // PrintWindow asks this thread to paint, so a blocking wait deadlocks.
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
    destroyEditorWhenIdle(dockedEditor, dockedHost);
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
    if (dockedEditor == nullptr || eqFocused || dynamicsFocused || focusedEffectWindow != nullptr) return;
    if (dockedEditor->getParentComponent() != &editorHolder) return;
    dockedEditor->setVisible(true);
    resized();
}

bool RackView::pluginHostAlive(PluginHost* host) const {
    if (host == nullptr) return false;
    for (const auto& ch : audioEngine.getChannels()) {
        for (const auto& insert : ch->inserts) {
            if (&insert->pluginHost == host) return true;
        }
    }
    for (const auto& out : audioEngine.getOutputs()) {
        for (const auto& insert : out->inserts) {
            if (&insert->pluginHost == host) return true;
        }
    }
    for (const auto& bus : audioEngine.getBuses()) {
        for (const auto& insert : bus->inserts) {
            if (&insert->pluginHost == host) return true;
        }
    }
    return false;
}

bool RackView::dockedHostAlive() const {
    return pluginHostAlive(dockedHost);
}

void RackView::focusEq() {
    closeDynamicsWindow();
    if (eqWindow != nullptr) {
        eqFocused = true;
        hideDockedEditor();
        if (eqWindow->getParentComponent() != &editorHolder)
            editorHolder.addAndMakeVisible(eqWindow);
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
            if (auto* c = safe->currentChannel())
                c->eqTap.snapshot(postEq, destL, destR, EQWindow::kAnalyzerFftSize);
        };
        window->setAnalyzerTapMode = [safe](int mode) {
            if (safe == nullptr) return;
            if (auto* c = safe->currentChannel())
                c->eqTap.setMode(static_cast<std::uint32_t>(mode));
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
        };
        eqWindow = window;
        eqFocused = true;
        hideDockedEditor();
        editorHolder.addAndMakeVisible(eqWindow);
        window->onCloseRequest = [safe] {
            if (safe == nullptr) return;
            safe->closeEqWindow();
        };
        resized();
    } else if (auto* out = currentOutput()) {
        const std::uint32_t outputId = (std::uint32_t)selectedId;
        juce::Component::SafePointer<RackView> safe(this);
        const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
        auto* window = new EQWindow(
            "EQ - " + out->name, out->eqBands,
            [safe, outputId](const std::vector<pluginrack::EqBand>& bands) {
                if (safe == nullptr) return;
                safe->audioEngine.setOutputEqBypassed(outputId, false);
                safe->audioEngine.setOutputEq(outputId, bands);
            },
            liveRateHz, true, out->eqRtaOffsetDb, out->eqRtaWeighting);
        window->applySpectrumOffset = [safe, outputId](float offsetDb) {
            if (safe == nullptr) return;
            safe->audioEngine.setOutputEqRtaOffset(outputId, offsetDb);
        };
        window->applyWeighting = [safe, outputId](int weighting) {
            if (safe == nullptr) return;
            safe->audioEngine.setOutputEqRtaWeighting(outputId, weighting);
        };
        window->pullAnalyzerTap = [safe](bool postEq, float* destL, float* destR) {
            if (safe == nullptr) return;
            if (auto* o = safe->currentOutput())
                o->eqTap.snapshot(postEq, destL, destR, EQWindow::kAnalyzerFftSize);
        };
        window->setAnalyzerTapMode = [safe](int mode) {
            if (safe == nullptr) return;
            if (auto* o = safe->currentOutput())
                o->eqTap.setMode(static_cast<std::uint32_t>(mode));
        };
        window->setAnalyzerMode(EQWindow::kAnalyzerPost);
        // Delay knob, same contract as the channel branch above.
        window->setDelayMs(out->eqDelayMs);
        window->applyDelayMs = [safe, outputId](float delayMs) {
            if (safe == nullptr) return;
            safe->audioEngine.setOutputEqBypassed(outputId, false);
            safe->audioEngine.setOutputEqDelay(outputId, delayMs);
        };
        eqWindow = window;
        eqFocused = true;
        hideDockedEditor();
        editorHolder.addAndMakeVisible(eqWindow);
        window->onCloseRequest = [safe] {
            if (safe == nullptr) return;
            safe->closeEqWindow();
        };
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
    if (focusedEffectWindow == nullptr) showDockedEditor();
}

void RackView::closeEffectWindows() {
    if (focusedEffectWindow == nullptr) return;
    if (feedbackSuppressorWindow != nullptr) {
        if (auto* c = currentChannel()) c->feedbackTap.setMode(AudioEngine::AnalyzerTap::kOff);
        if (auto* o = currentOutput()) o->feedbackTap.setMode(AudioEngine::AnalyzerTap::kOff);
    }
    auto* doomed = focusedEffectWindow;
    focusedEffectWindow = nullptr;
    multibandWindow = nullptr;
    deEsserWindow = nullptr;
    limiterWindow = nullptr;
    feedbackSuppressorWindow = nullptr;    spectralDenoiserWindow = nullptr;    eqFocused = false;
    dynamicsFocused = false;
    editorHolder.removeChildComponent(doomed);
    delete doomed;
    showDockedEditor();
}

void RackView::focusMultiband() {
    closeEffectWindows();
    closeEqWindow();
    closeDynamicsWindow();
    auto* channel = currentChannel();
    if (channel == nullptr) return;

    juce::Component::SafePointer<RackView> safe(this);
    if (channel->multibandEffects.empty()) {
        auto effects = channel->multibandEffects;
        effects.push_back(pluginrack::makeDefaultMultibandEffect(1));
        audioEngine.setChannelMultiband(channel->id, effects);
        channel->chain.push_back({pluginrack::ChainItemType::Multiband, 1});
        audioEngine.ensureChannelChainItem(channel->id, {pluginrack::ChainItemType::Multiband, 1});
    }
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
    auto* window = new MultibandWindow(
        "Multiband Compressor - " + channel->name, channel->multibandEffects.front(),
        [safe](const pluginrack::MultibandEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto* currentChannel = safe->currentChannel();
            if (currentChannel == nullptr) return;
            auto effects = currentChannel->multibandEffects;
            effectedit::applyEdit(effects, effect, bypassChanged);
            safe->audioEngine.setChannelMultiband(currentChannel->id, effects);
        }, channel->multibandRtaOffsetDb, channel->multibandRtaWeighting, liveRateHz);
    window->applySpectrumOffset = [safe](float offsetDb) {
        if (safe != nullptr && safe->currentChannel() != nullptr)
            safe->audioEngine.setChannelMultibandRtaOffset(safe->currentChannel()->id, offsetDb);
    };
    window->applyWeighting = [safe](int weighting) {
        if (safe != nullptr && safe->currentChannel() != nullptr)
            safe->audioEngine.setChannelMultibandRtaWeighting(safe->currentChannel()->id, weighting);
    };
    channel->multibandTap.setMode(AudioEngine::AnalyzerTap::kPost);
    window->pullAnalyzerTap = [safe](float* l, float* r, int n) {
        if (safe != nullptr && safe->currentChannel() != nullptr)
            safe->currentChannel()->multibandTap.snapshot(true, l, r, n);
    };
    window->onClose = [safe] { if (safe != nullptr) safe->closeEffectWindows(); };
    multibandWindow = window;
    focusedEffectWindow = window;
    hideDockedEditor();
    editorHolder.addAndMakeVisible(window);
    rebuild();
    resized();
}

void RackView::focusDeEsser() {
    closeEffectWindows();
    closeEqWindow();
    closeDynamicsWindow();
    auto* channel = currentChannel();
    if (channel == nullptr) return;

    juce::Component::SafePointer<RackView> safe(this);
    if (channel->deEsserEffects.empty()) {
        auto effects = channel->deEsserEffects;
        effects.push_back(pluginrack::makeDefaultDeEsserEffect(1));
        audioEngine.setChannelDeEssers(channel->id, effects);
        channel->chain.push_back({pluginrack::ChainItemType::DeEsser, 1});
        audioEngine.ensureChannelChainItem(channel->id, {pluginrack::ChainItemType::DeEsser, 1});
    }
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
    auto* window = new DeEsserWindow(
        "De-esser - " + channel->name, channel->deEsserEffects.front(),
        [safe](const pluginrack::DeEsserEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto* currentChannel = safe->currentChannel();
            if (currentChannel == nullptr) return;
            auto effects = currentChannel->deEsserEffects;
            effectedit::applyEdit(effects, effect, bypassChanged);
            safe->audioEngine.setChannelDeEssers(currentChannel->id, effects);
        }, channel->deEsserRtaOffsetDb, channel->deEsserRtaWeighting, liveRateHz);
    window->applySpectrumOffset = [safe](float offsetDb) {
        if (safe != nullptr && safe->currentChannel() != nullptr)
            safe->audioEngine.setChannelDeEsserRtaOffset(safe->currentChannel()->id, offsetDb);
    };
    window->applyWeighting = [safe](int weighting) {
        if (safe != nullptr && safe->currentChannel() != nullptr)
            safe->audioEngine.setChannelDeEsserRtaWeighting(safe->currentChannel()->id, weighting);
    };
    channel->deEsserTap.setMode(AudioEngine::AnalyzerTap::kPost);
    window->pullAnalyzerTap = [safe](float* l, float* r, int n) {
        if (safe != nullptr && safe->currentChannel() != nullptr)
            safe->currentChannel()->deEsserTap.snapshot(true, l, r, n);
    };
    window->onClose = [safe] { if (safe != nullptr) safe->closeEffectWindows(); };
    deEsserWindow = window;
    focusedEffectWindow = window;
    hideDockedEditor();
    editorHolder.addAndMakeVisible(window);
    rebuild();
    resized();
}

void RackView::focusLimiter() {
    closeEffectWindows();
    closeEqWindow();
    closeDynamicsWindow();
    auto* channel = currentChannel();
    if (channel == nullptr) return;

    juce::Component::SafePointer<RackView> safe(this);
    if (channel->limiterEffects.empty()) {
        auto effects = channel->limiterEffects;
        effects.push_back(pluginrack::makeDefaultLimiterEffect(1));
        audioEngine.setChannelLimiters(channel->id, effects);
        channel->chain.push_back({pluginrack::ChainItemType::Limiter, 1});
        audioEngine.ensureChannelChainItem(channel->id, {pluginrack::ChainItemType::Limiter, 1});
    }
    auto* window = new LimiterWindow(
        "Limiter - " + channel->name, channel->limiterEffects.front(),
        [safe](const pluginrack::LimiterEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto* currentChannel = safe->currentChannel();
            if (currentChannel == nullptr) return;
            auto effects = currentChannel->limiterEffects;
            effectedit::applyEdit(effects, effect, bypassChanged);
            safe->audioEngine.setChannelLimiters(currentChannel->id, effects);
        });
    channel->limiterTap.setMode(AudioEngine::AnalyzerTap::kPost);
    window->pullAnalyzerTap = [safe](bool /*postEq*/, float* l, float* r, int n) {
        if (safe != nullptr && safe->currentChannel() != nullptr)
            safe->currentChannel()->limiterTap.snapshot(true, l, r, n);
    };
    window->onClose = [safe] { if (safe != nullptr) safe->closeEffectWindows(); };
    limiterWindow = window;
    focusedEffectWindow = window;
    hideDockedEditor();
    editorHolder.addAndMakeVisible(window);
    resized();
}

void RackView::focusFeedbackSuppressor() {
    closeEffectWindows();
    closeEqWindow();
    closeDynamicsWindow();
    auto* channel = currentChannel();
    if (channel == nullptr) return;
    juce::Component::SafePointer<RackView> safe(this);
    if (channel->feedbackSuppressorEffects.empty()) {
        auto effects = channel->feedbackSuppressorEffects;
        effects.push_back(pluginrack::makeDefaultFeedbackSuppressorEffect(1));
        channel->feedbackSuppressorEffects = effects;
        audioEngine.setChannelFeedbackSuppressors(channel->id, effects);
        channel->chain.push_back({pluginrack::ChainItemType::FeedbackSuppressor, 1});
        audioEngine.ensureChannelChainItem(channel->id, {pluginrack::ChainItemType::FeedbackSuppressor, 1});
    }
    const double liveRateHz = audioEngine.getLatencyReport().sampleRate;
    auto* window = new FeedbackSuppressorWindow(
        "Feedback Suppressor - " + channel->name, channel->feedbackSuppressorEffects.front(),
        [safe](const pluginrack::FeedbackSuppressorEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            auto* currentChannel = safe->currentChannel();
            if (currentChannel == nullptr) return;
            auto effects = currentChannel->feedbackSuppressorEffects;
            const auto it = std::find_if(effects.begin(), effects.end(), [&effect](const auto& e) {
                return e.id == effect.id;
            });
            if (it == effects.end())
                effects.push_back(effect);
            else
                effectedit::applyEdit(effects, effect, bypassChanged);
            currentChannel->feedbackSuppressorEffects = effects;
            safe->audioEngine.setChannelFeedbackSuppressors(currentChannel->id, effects);
        }, channel->feedbackSuppressorRtaOffsetDb, channel->feedbackSuppressorRtaWeighting,
        liveRateHz);
    window->applySpectrumOffset = [safe](float offsetDb) {
        if (safe == nullptr || safe->currentChannel() == nullptr) return;
        safe->audioEngine.setChannelFeedbackSuppressorRtaOffset(safe->currentChannel()->id, offsetDb);
    };
    window->applyWeighting = [safe](int weighting) {
        if (safe == nullptr || safe->currentChannel() == nullptr) return;
        safe->audioEngine.setChannelFeedbackSuppressorRtaWeighting(safe->currentChannel()->id, weighting);
    };
    channel->feedbackTap.setMode(AudioEngine::AnalyzerTap::kPost);
    window->pullAnalyzerTap = [safe](bool postEq, float* l, float* r, int n) {
        if (safe != nullptr && safe->currentChannel() != nullptr)
            safe->currentChannel()->feedbackTap.snapshot(postEq, l, r, n);
    };
    window->pullNotches = [safe](float* f, float* d, int n) {
        if (safe == nullptr || safe->currentChannel() == nullptr) return 0;
        return safe->audioEngine.getChannelFeedbackNotches(safe->currentChannel()->id, f, d, n);
    };
    window->onClose = [safe] {
        if (safe == nullptr) return;
        if (auto* c = safe->currentChannel()) c->feedbackTap.setMode(AudioEngine::AnalyzerTap::kOff);
        if (auto* o = safe->currentOutput()) o->feedbackTap.setMode(AudioEngine::AnalyzerTap::kOff);
        safe->closeEffectWindows();
    };
    feedbackSuppressorWindow = window;
    focusedEffectWindow = window;
    hideDockedEditor();
    editorHolder.addAndMakeVisible(window);
    resized();
}


void RackView::focusSpectralDenoiser() {
    closeEffectWindows();
    closeEqWindow();
    closeDynamicsWindow();

    juce::Component::SafePointer<RackView> safe(this);
    const bool isOutput = selectedIsOutput;
    std::uint32_t stripId = 0;
    const pluginrack::SpectralDenoiserEffect* initial = nullptr;

    if (isOutput) {
        auto* output = currentOutput();
        if (output == nullptr) return;
        if (output->spectralDenoiserEffects.empty()) {
            auto effects = output->spectralDenoiserEffects;
            effects.push_back(pluginrack::makeDefaultSpectralDenoiserEffect(1));
            output->spectralDenoiserEffects = effects;
            audioEngine.setOutputSpectralDenoisers(output->id, effects);
            output->chain.push_back({pluginrack::ChainItemType::SpectralDenoiser, 1});
            audioEngine.ensureOutputChainItem(output->id, {pluginrack::ChainItemType::SpectralDenoiser, 1});
        }
        stripId = output->id;
        initial = &output->spectralDenoiserEffects.front();
    } else {
        auto* channel = currentChannel();
        if (channel == nullptr) return;
        if (channel->spectralDenoiserEffects.empty()) {
            auto effects = channel->spectralDenoiserEffects;
            effects.push_back(pluginrack::makeDefaultSpectralDenoiserEffect(1));
            channel->spectralDenoiserEffects = effects;
            audioEngine.setChannelSpectralDenoisers(channel->id, effects);
            channel->chain.push_back({pluginrack::ChainItemType::SpectralDenoiser, 1});
            audioEngine.ensureChannelChainItem(channel->id, {pluginrack::ChainItemType::SpectralDenoiser, 1});
        }
        stripId = channel->id;
        initial = &channel->spectralDenoiserEffects.front();
    }

    auto* window = new SpectralDenoiserWindow(
        "Spectral Denoiser - " + juce::String(isOutput ? currentOutput()->name : currentChannel()->name),
        *initial, audioEngine, isOutput, stripId,
        [safe, isOutput](const pluginrack::SpectralDenoiserEffect& effect, bool bypassChanged) {
            if (safe == nullptr) return;
            if (isOutput) {
                auto* output = safe->currentOutput();
                if (output == nullptr) return;
                auto effects = output->spectralDenoiserEffects;
                const auto it = std::find_if(effects.begin(), effects.end(),
                    [&effect](const auto& e) { return e.id == effect.id; });
                if (it == effects.end()) effects.push_back(effect);
                else effectedit::applyEdit(effects, effect, bypassChanged);
                output->spectralDenoiserEffects = effects;
                safe->audioEngine.setOutputSpectralDenoisers(output->id, effects);
            } else {
                auto* channel = safe->currentChannel();
                if (channel == nullptr) return;
                auto effects = channel->spectralDenoiserEffects;
                const auto it = std::find_if(effects.begin(), effects.end(),
                    [&effect](const auto& e) { return e.id == effect.id; });
                if (it == effects.end()) effects.push_back(effect);
                else effectedit::applyEdit(effects, effect, bypassChanged);
                channel->spectralDenoiserEffects = effects;
                safe->audioEngine.setChannelSpectralDenoisers(channel->id, effects);
            }
        });
    window->onClose = [safe] { if (safe != nullptr) safe->closeEffectWindows(); };
    spectralDenoiserWindow = window;
    focusedEffectWindow = window;
    hideDockedEditor();
    editorHolder.addAndMakeVisible(window);
    resized();
}

void RackView::focusDiagnosticsWindow(pluginrack::ChainItemType type) {
    if (type == pluginrack::ChainItemType::SpectralDenoiser) focusSpectralDenoiser();
    else if (type == pluginrack::ChainItemType::FeedbackSuppressor) focusFeedbackSuppressor();
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
                effectedit::applyEdit(effects, effect, bypassChanged);
                safe->audioEngine.setChannelEffects(channel->id, effects);
                safe->refreshDynamicsButtons();
            },
            gateIt != ch->effects.end() ? &*gateIt : nullptr);
        // The Dynamics RTA reads the compressor/gate output (dynamicsTap),
        // so it shows the dynamics' own signal, not the strip input.
        window->pullAnalyzerTap = [safe](float* left, float* right, int frames) {
            if (safe == nullptr) return;
            if (auto* channel = safe->currentChannel()) {
                channel->dynamicsTap.setMode(AudioEngine::AnalyzerTap::kPost);
                channel->dynamicsTap.snapshot(true, left, right, frames);
            }
        };
        ch->dynamicsTap.setMode(AudioEngine::AnalyzerTap::kPost);
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
                effectedit::applyEdit(effects, effect, bypassChanged);
                safe->audioEngine.setOutputEffects(output->id, effects);
                safe->refreshDynamicsButtons();
            },
            gateIt != out->effects.end() ? &*gateIt : nullptr);
        window->pullAnalyzerTap = [safe](float* left, float* right, int frames) {
            if (safe == nullptr) return;
            if (auto* output = safe->currentOutput()) {
                output->dynamicsTap.setMode(AudioEngine::AnalyzerTap::kPost);
                output->dynamicsTap.snapshot(true, left, right, frames);
            }
        };
        out->dynamicsTap.setMode(AudioEngine::AnalyzerTap::kPost);
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
        ch->dynamicsTap.setMode(AudioEngine::AnalyzerTap::kOff);
    } else if (auto* out = currentOutput()) {
        out->dynamicsTap.setMode(AudioEngine::AnalyzerTap::kOff);
    }
    editorHolder.removeChildComponent(doomed);
    delete doomed;
    refreshDynamicsButtons();
    showDockedEditor();
}


void RackView::refreshDynamicsButtons() {
    // No standalone buttons left: the Dynamics card carries the bypass
    // switches. Still syncs an open Dynamics window with the chain state.
    const std::vector<pluginrack::DynamicsEffect>* effects = nullptr;
    if (auto* ch = currentChannel()) effects = &ch->effects;
    else if (auto* out = currentOutput()) effects = &out->effects;
    if (effects == nullptr || dynamicsWindow == nullptr) return;
    const auto type = dynamicsWindow->getType();
    const auto found = std::find_if(effects->begin(), effects->end(),
                                    [type](const auto& effect) { return effect.type == type; });
    if (found == effects->end()) return;
    dynamicsWindow->setBypassed(found->bypassed);
    if (type == pluginrack::DynamicsEffectType::Compressor) {
        const auto gateIt = std::find_if(
            effects->begin(), effects->end(), [](const auto& effect) {
                return effect.type == pluginrack::DynamicsEffectType::Gate;
            });
        if (gateIt != effects->end()) dynamicsWindow->setGate(*gateIt);
    }
}

void RackView::SourceRowList::setRows(juce::StringArray channelNames,
                                      juce::StringArray deviceNames,
                                      const std::vector<float>& channelHues,
                                      juce::StringArray sectionHeaders) {
    if (names == channelNames && devices == deviceNames && hues == channelHues
        && headers == sectionHeaders)
        return;
    names = std::move(channelNames);
    devices = std::move(deviceNames);
    hues = channelHues;
    headers = std::move(sectionHeaders);
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
        if (i < headers.size() && headers[i].isNotEmpty()) {
            auto headerRow = area.removeFromTop(kHeaderH);
            if (i > 0) {
                g.setColour(theme::border());
                g.fillRect(headerRow.removeFromTop(1));
                headerRow.removeFromTop(3);
            }
            g.setColour(theme::dim());
            g.setFont(theme::ui::captionFont());
            g.drawFittedText(headers[i], headerRow, juce::Justification::centredLeft, 1);
            continue;
        }
        auto row = area.removeFromTop(kRowH);
        const float hue = (i < (int)hues.size()) ? hues[static_cast<std::size_t>(i)] : 0.0f;
        const auto accent = theme::stripColour(hue);
        g.setColour(accent);
        g.fillRoundedRectangle(row.toFloat().reduced(0.0f, 1.0f), 3.0f);

        auto text = row.reduced(6, 3);
        g.setColour(theme::bg());
        g.setFont(theme::ui::bodyBoldFont());
        g.drawFittedText(names[i], text.removeFromTop(15), juce::Justification::centredLeft, 1);
        g.setColour(theme::bg().withAlpha(0.75f));
        g.setFont(theme::ui::captionFont());
        g.drawFittedText(devices[i], text, juce::Justification::centredLeft, 1);
        area.removeFromTop(2);
    }
}

void RackView::refreshSourceList() {
    juce::StringArray sourceNames;
    juce::StringArray sourceDevices;
    juce::StringArray sourceHeaders;
    std::vector<float> sourceHues;
    const auto addHeader = [&](const juce::String& text) {
        sourceNames.add(juce::String{});
        sourceDevices.add(juce::String{});
        sourceHues.push_back(0.0f);
        sourceHeaders.add(text);
    };
    const auto addRow = [&](const juce::String& name, float hue, const juce::String& device) {
        sourceNames.add(name);
        sourceHues.push_back(hue);
        sourceDevices.add(device);
        sourceHeaders.add({});
    };
    if (const auto* out = currentOutput()) {
        if (isSelectedBus()) {
            // A bus is fed by bus sends, not channel->output routes, so list
            // the channels whose send into this bus is above the floor.
            addHeader("Inputs");
            for (const auto& send : audioEngine.getBusSends()) {
                if (send->busId != out->id) continue;
                if (send->sendGainDbL.get() <= AudioEngine::kSendOffDb
                    && send->sendGainDbR.get() <= AudioEngine::kSendOffDb)
                    continue;
                for (const auto& channel : audioEngine.getChannels()) {
                    if (channel->id != send->channelId) continue;
                    const auto device = audioEngine.getChannelDeviceState(channel->id).deviceName;
                    addRow(channel->name, channel->colourHue,
                           device.isNotEmpty() ? device : juce::String("No input device"));
                }
            }
            // The bus's own destinations: outputs it is routed to.
            addHeader("Outputs");
            for (const auto& route : audioEngine.getBusRoutes()) {
                if (route->busId != out->id) continue;
                if (route->sendGainDbL.get() <= AudioEngine::kSendOffDb
                    && route->sendGainDbR.get() <= AudioEngine::kSendOffDb)
                    continue;
                for (const auto& target : audioEngine.getOutputs()) {
                    if (target->id != route->outputId) continue;
                    const auto device = audioEngine.getOutputDeviceState(target->id).deviceName;
                    addRow(target->name, target->colourHue,
                           device.isNotEmpty() ? device : juce::String("No output device"));
                }
            }
        } else {
            for (const auto& channel : audioEngine.getChannels()) {
                if (audioEngine.getSendGain(channel->id, out->id) <= -30.0f) continue;
                const auto device = audioEngine.getChannelDeviceState(channel->id).deviceName;
                addRow(channel->name, channel->colourHue,
                       device.isNotEmpty() ? device : juce::String("No input device"));
            }
        }
    }
    sourceList.setRows(std::move(sourceNames), std::move(sourceDevices),
                       std::move(sourceHues), std::move(sourceHeaders));
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
    outputHeader.setText(isSelectedBus() ? "Inputs / Outputs"
                                         : (out != nullptr ? "Inputs" : "Output"),
                         juce::dontSendNotification);

    for (size_t i = 0; i < outputs.size(); ++i) {
        auto* btn = outputButtons.add(new juce::TextButton(outputs[i]->name));
        btn->setClickingTogglesState(true);
        btn->setTooltip(outputs[i]->name);
        btn->setColour(juce::TextButton::buttonColourId, theme::control());
        // Active send lights in the output's own identity colour.
        btn->setColour(juce::TextButton::buttonOnColourId,
                       theme::stripColour(outputs[i]->colourHue));
        btn->setColour(juce::TextButton::textColourOffId, theme::text());
        btn->setColour(juce::TextButton::textColourOnId, theme::bg());
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
    // Every preview editor is owned by an insert's PluginHost: a scene
    // restore or an outside-rack erase destroys those hosts before this
    // rebuild runs, so retire all previews first. Otherwise a stale editor
    // blocks the fresh one JUCE allows per processor, or its host pointer
    // dangles into a freed insert.
    clearHiddenPreviews();
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
    // Built-in effects and VST inserts share one ordered list in the rack.
    pa.removeFromTop(14);
    insertsHeader.setBounds(pa.removeFromTop(16));
    pa.removeFromTop(4);
    slotViewport.setBounds(pa);
    layoutSlots();

    // Keep embedded editors inside the framed center bay. The viewport and
    // its child editors paint opaque surfaces, so using the panel bounds
    // directly would cover the panel outline. A focused VST insert also gets
    // the side-chain strip pinned above its editor.
    updateSideChainBar();
    auto editorArea = centerArea.reduced(2);
    if (sideChainBarVisible) {
        sideChainBar.setBounds(editorArea.removeFromTop(SideChainBar::kHeight));
        editorArea.removeFromTop(4);
    }
    editorViewport.setBounds(editorArea);
    const int vw = juce::jmax(0, editorViewport.getWidth());
    const int vh = juce::jmax(0, editorViewport.getHeight());
    // Every built-in window is the same 860x560 panel. The panel is centred
    // vertically in the bay (its holder is shifted by the spare space above
    // and below) instead of top-aligned, so the window never sits against the
    // top edge with a dead strip below it, and switching between windows never
    // changes the height or leaves a black bar behind. The gate fits in one
    // 132 px row below the compressor knobs, so all five windows share the
    // same panel and the holder never jumps.
    auto centreInBay = [&](int width, int height) {
        const int holderW = juce::jmax(vw, width);
        const int holderH = juce::jmax(vh, height);
        const int panelW = juce::jmin(width, holderW);
        const int panelH = juce::jmin(height, holderH);
        editorHolder.setSize(holderW, holderH);
        const int x = (holderW - panelW) / 2;
        const int y = (holderH - panelH) / 2;
        editorHolder.setTopLeftPosition(x, y);
    };
    if (eqFocused && eqWindow != nullptr) {
        centreInBay(EQWindow::kWidth, EQWindow::kHeight);
        eqWindow->setBounds(editorHolder.getLocalBounds());
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
    }
    if (dynamicsFocused && dynamicsWindow != nullptr) {
        centreInBay(DynamicsWindow::kMinWidth, DynamicsWindow::kMinHeight);
        dynamicsWindow->setBounds(editorHolder.getLocalBounds());
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
    }
    if (focusedEffectWindow != nullptr) {
        // Fill the rack editor bay edge to edge, like the EQ/Dynamics
        // panels: the graph and knobs stretch to the whole width instead
        // of sitting in a fixed 860px holder with dead space on the right
        // (was the "Multiband/De-esser/Limiter not full width" report).
        // Height keeps the windows' compact 560px; only the width stretches.
        centreInBay(860, 560);
        focusedEffectWindow->setBounds(editorHolder.getLocalBounds());
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
    }
    if (dockedEditor != nullptr && focusedEffectWindow == nullptr) {
        // The bay is the allocation: keep the holder at the visible size; the
        // layout scales the plugin to fit that whole area.
        editorHolder.setSize(juce::jmax(0, vw), juce::jmax(0, vh));
        layoutDockedEditor();
        noEditorLabel.setBounds(editorHolder.getLocalBounds());
    } else if (focusedEffectWindow == nullptr) {
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

void RackView::paintOverChildren(juce::Graphics& g) {
    if (!frozenBayActive || frozenBayImage.isNull()) return;
    // The frozen frame is painted last: both native editors are hidden while a
    // swap is in flight, so this is what the bay shows instead of going black.
    const auto target = editorHolder.getBounds();
    g.drawImage(frozenBayImage, target.toFloat(),
                juce::RectanglePlacement::stretchToFit, false);
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

    // Channel identity colour: the left strip panel carries the selected
    // channel/output's accent along its bottom edge, like the mixer strips.
    // Bounds (not raw width/height) so the bar lands on the panel's own
    // edge instead of the view's — the old version drew from x=0 and
    // overlapped the button row.
    if (const auto* ch = currentChannel())
        theme::drawStripAccent(g, leftArea, ch->colourHue);
    else if (const auto* out = currentOutput())
        theme::drawStripAccent(g, leftArea, out->colourHue);
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
    float currentHue = 0.0f;
    const int selected = selectedId;
    const bool isOut = selectedIsOutput;
    if (auto* ch = currentChannel()) {
        currentName = ch->name;
        currentHue = ch->colourHue;
    } else if (auto* out = currentOutput()) {
        currentName = out->name;
        currentHue = out->colourHue;
    }

    juce::Component::SafePointer<RackView> safe(this);
    StripDialog::show("Rename", currentName, currentHue,
                      [safe, selected, isOut](const juce::String& newName, float newHue) {
        if (safe == nullptr || newName.trim().isEmpty()) return;
        if (!isOut) {
            safe->audioEngine.setChannelName(static_cast<std::uint32_t>(selected), newName);
            safe->audioEngine.setChannelColourHue(static_cast<std::uint32_t>(selected), newHue);
        } else {
            safe->audioEngine.setOutputName(static_cast<std::uint32_t>(selected), newName);
            safe->audioEngine.setOutputColourHue(static_cast<std::uint32_t>(selected), newHue);
        }
        safe->rebuild();
    });
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

void RackView::reloadPlugin(std::uint32_t insertId) {
    if (auto* ch = currentChannel()) audioEngine.reloadInsert(ch->id, insertId);
    else if (auto* out = currentOutput()) audioEngine.reloadInsert(out->id, insertId);
    refreshSlots();
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
            audioEngine.setOutputEqBypassed(static_cast<std::uint32_t>(selectedId), bypassed);
        } else {
            audioEngine.setChannelEqBypassed(static_cast<std::uint32_t>(selectedId), bypassed);
        }
    } else if (item.type == pluginrack::ChainItemType::Multiband) {
        if (selectedIsOutput) {
            if (auto* output = currentOutput()) {
                auto effects = output->multibandEffects;
                effectedit::setBypass(effects, item.id, bypassed);
                audioEngine.setOutputMultiband(output->id, effects);
            }
        } else if (auto* channel = currentChannel()) {
            auto effects = channel->multibandEffects;
            effectedit::setBypass(effects, item.id, bypassed);
            audioEngine.setChannelMultiband(channel->id, effects);
        }
        if (multibandWindow != nullptr) multibandWindow->setBypassed(bypassed);
    } else if (item.type == pluginrack::ChainItemType::DeEsser) {
        if (selectedIsOutput) {
            if (auto* output = currentOutput()) {
                auto effects = output->deEsserEffects;
                effectedit::setBypass(effects, item.id, bypassed);
                audioEngine.setOutputDeEssers(output->id, effects);
            }
        } else if (auto* channel = currentChannel()) {
            auto effects = channel->deEsserEffects;
            effectedit::setBypass(effects, item.id, bypassed);
            audioEngine.setChannelDeEssers(channel->id, effects);
        }
        if (deEsserWindow != nullptr) deEsserWindow->setBypassed(bypassed);
    } else if (item.type == pluginrack::ChainItemType::Limiter) {
        if (selectedIsOutput) {
            if (auto* output = currentOutput()) {
                auto effects = output->limiterEffects;
                effectedit::setBypass(effects, item.id, bypassed);
                audioEngine.setOutputLimiters(output->id, effects);
            }
        } else if (auto* channel = currentChannel()) {
            auto effects = channel->limiterEffects;
            effectedit::setBypass(effects, item.id, bypassed);
            audioEngine.setChannelLimiters(channel->id, effects);
        }
        if (limiterWindow != nullptr) limiterWindow->setBypassed(bypassed);
    } else if (item.type == pluginrack::ChainItemType::FeedbackSuppressor) {
        if (selectedIsOutput) {
            if (auto* output = currentOutput()) {
                auto effects = output->feedbackSuppressorEffects;
                effectedit::setBypass(effects, item.id, bypassed);
                audioEngine.setOutputFeedbackSuppressors(output->id, effects);
            }
        } else if (auto* channel = currentChannel()) {
            auto effects = channel->feedbackSuppressorEffects;
            effectedit::setBypass(effects, item.id, bypassed);
            audioEngine.setChannelFeedbackSuppressors(channel->id, effects);
        }
    } else if (item.type == pluginrack::ChainItemType::SpectralDenoiser) {
        if (selectedIsOutput) {
            if (auto* output = currentOutput()) {
                auto effects = output->spectralDenoiserEffects;
                effectedit::setBypass(effects, item.id, bypassed);
                audioEngine.setOutputSpectralDenoisers(output->id, effects);
            }
        } else if (auto* channel = currentChannel()) {
            auto effects = channel->spectralDenoiserEffects;
            effectedit::setBypass(effects, item.id, bypassed);
            audioEngine.setChannelSpectralDenoisers(channel->id, effects);
        }
        if (spectralDenoiserWindow != nullptr) spectralDenoiserWindow->setBypassed(bypassed);
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

        effectedit::setBypass(effects, targetId, bypassed);
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
    else if (item.type == pluginrack::ChainItemType::Dynamics) focusDynamics(pluginrack::DynamicsEffectType::Compressor);
    else if (item.type == pluginrack::ChainItemType::Multiband) focusMultiband();
    else if (item.type == pluginrack::ChainItemType::DeEsser) focusDeEsser();
    else if (item.type == pluginrack::ChainItemType::Limiter) focusLimiter();
    else if (item.type == pluginrack::ChainItemType::FeedbackSuppressor) focusFeedbackSuppressor();
    else if (item.type == pluginrack::ChainItemType::SpectralDenoiser) focusSpectralDenoiser();
}

void RackView::insertBuiltIn(pluginrack::ChainItem item, int chainIndex) {
    if (selectedIsOutput) {
        if (auto* output = currentOutput()) audioEngine.ensureOutputChainItem(output->id, item);
    } else if (auto* channel = currentChannel()) {
        audioEngine.ensureChannelChainItem(channel->id, item);
    }
    if (selectedIsOutput) {
        if (auto* output = currentOutput()) {
            if (item.type == pluginrack::ChainItemType::Multiband && std::none_of(output->multibandEffects.begin(), output->multibandEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = output->multibandEffects; fx.push_back(pluginrack::makeDefaultMultibandEffect(item.id)); audioEngine.setOutputMultiband(output->id, fx); }
            if (item.type == pluginrack::ChainItemType::DeEsser && std::none_of(output->deEsserEffects.begin(), output->deEsserEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = output->deEsserEffects; fx.push_back(pluginrack::makeDefaultDeEsserEffect(item.id)); audioEngine.setOutputDeEssers(output->id, fx); }
            if (item.type == pluginrack::ChainItemType::Limiter && std::none_of(output->limiterEffects.begin(), output->limiterEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = output->limiterEffects; fx.push_back(pluginrack::makeDefaultLimiterEffect(item.id)); audioEngine.setOutputLimiters(output->id, fx); }
            if (item.type == pluginrack::ChainItemType::FeedbackSuppressor && std::none_of(output->feedbackSuppressorEffects.begin(), output->feedbackSuppressorEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = output->feedbackSuppressorEffects; fx.push_back(pluginrack::makeDefaultFeedbackSuppressorEffect(item.id)); audioEngine.setOutputFeedbackSuppressors(output->id, fx); }
            if (item.type == pluginrack::ChainItemType::SpectralDenoiser && std::none_of(output->spectralDenoiserEffects.begin(), output->spectralDenoiserEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = output->spectralDenoiserEffects; fx.push_back(pluginrack::makeDefaultSpectralDenoiserEffect(item.id)); audioEngine.setOutputSpectralDenoisers(output->id, fx); }
        }
    } else if (auto* channel = currentChannel()) {
        if (item.type == pluginrack::ChainItemType::Multiband && std::none_of(channel->multibandEffects.begin(), channel->multibandEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = channel->multibandEffects; fx.push_back(pluginrack::makeDefaultMultibandEffect(item.id)); audioEngine.setChannelMultiband(channel->id, fx); }
        if (item.type == pluginrack::ChainItemType::DeEsser && std::none_of(channel->deEsserEffects.begin(), channel->deEsserEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = channel->deEsserEffects; fx.push_back(pluginrack::makeDefaultDeEsserEffect(item.id)); audioEngine.setChannelDeEssers(channel->id, fx); }
        if (item.type == pluginrack::ChainItemType::Limiter && std::none_of(channel->limiterEffects.begin(), channel->limiterEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = channel->limiterEffects; fx.push_back(pluginrack::makeDefaultLimiterEffect(item.id)); audioEngine.setChannelLimiters(channel->id, fx); }
        if (item.type == pluginrack::ChainItemType::FeedbackSuppressor && std::none_of(channel->feedbackSuppressorEffects.begin(), channel->feedbackSuppressorEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = channel->feedbackSuppressorEffects; fx.push_back(pluginrack::makeDefaultFeedbackSuppressorEffect(item.id)); audioEngine.setChannelFeedbackSuppressors(channel->id, fx); }
        if (item.type == pluginrack::ChainItemType::SpectralDenoiser && std::none_of(channel->spectralDenoiserEffects.begin(), channel->spectralDenoiserEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = channel->spectralDenoiserEffects; fx.push_back(pluginrack::makeDefaultSpectralDenoiserEffect(item.id)); audioEngine.setChannelSpectralDenoisers(channel->id, fx); }
    }
    if (item.type == pluginrack::ChainItemType::Dynamics) {
        if (const auto* effects = currentEffects()) {
            for (const auto& effect : *effects)
                if (effect.id != item.id) {
                    if (selectedIsOutput) {
                        if (auto* output = currentOutput())
                            audioEngine.ensureOutputChainItem(
                                output->id, {pluginrack::ChainItemType::Dynamics, effect.id});
                    } else if (auto* channel = currentChannel()) {
                        audioEngine.ensureChannelChainItem(
                            channel->id, {pluginrack::ChainItemType::Dynamics, effect.id});
                    }
                }
        }
    }
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
    moveChainCard(cardItems, chainIndex);
    setBuiltInBypass(item, false);
    refreshAfterInsertChange();
}

void RackView::eraseBuiltIn(pluginrack::ChainItem item) {
    // The Dynamics card is one node: removing it takes both effects, so
    // expand to the full pair like the move path does.
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
    for (const auto& entry : cardItems) {
        if (selectedIsOutput) {
            if (auto* output = currentOutput())
                audioEngine.removeOutputChainItem(output->id, entry);
        } else if (auto* channel = currentChannel()) {
            audioEngine.removeChannelChainItem(channel->id, entry);
        }
    }
    if (item.type == pluginrack::ChainItemType::FeedbackSuppressor
        && feedbackSuppressorWindow != nullptr) {
        if (auto* c = currentChannel()) c->feedbackTap.setMode(AudioEngine::AnalyzerTap::kOff);
        if (auto* o = currentOutput()) o->feedbackTap.setMode(AudioEngine::AnalyzerTap::kOff);
        closeEffectWindows();
    }
    if (item.type == pluginrack::ChainItemType::SpectralDenoiser
        && spectralDenoiserWindow != nullptr) {
        closeEffectWindows();
    }
    refreshAfterInsertChange();
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
    // Same ownership rule as rebuild(): the insert list changed, so any
    // preview editor may belong to a removed insert.
    clearHiddenPreviews();
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
    // Clicking a slot opens that insert in the bay: clear every docked
    // built-in first, or a stuck Multiband/De-esser/Limiter keeps the bay
    // and the VST editor never appears.
    closeEffectWindows();
    closeEqWindow();
    closeDynamicsWindow();
    // The newly focused insert gets the bay's live editor; a hidden editor
    // for the same insert would be a second owner, which JUCE forbids.
    retireHiddenPreview(insert->id);
    focusedInsertId = insert->id;
    refreshEditor();
    return true;
}

