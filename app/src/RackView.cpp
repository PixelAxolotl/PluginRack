#include "RackView.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "MultibandCurveComponent.h"
#include "NativeThumbnail.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

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
    // Placeholder for the embedded host window (see the header): hidden
    // until an editor is assigned, never intercepts clicks itself — the
    // native window on top takes input.
    dockedHwndView.setVisible(false);
    dockedHwndView.setInterceptsMouseClicks(false, false);
    editorHolder.addChildComponent(&dockedHwndView);

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
    clearBackgroundPreview();
    clearAllLivePreviews();
    closeDockedEditor();
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
            slot->setInsert(found);
            auto mini = std::make_unique<InsertMiniView>();
            mini->showVst();
            // Reuse the last snapshot so a rebuilt card does not flash back
            // to "open to capture preview" while the next capture runs.
            if (auto thumb = vstThumbs.find(found->id); thumb != vstThumbs.end())
                mini->setVstSnapshot(thumb->second);
            slot->setMiniView(std::move(mini));
            slot->setSelected(focusedInsertId == found->id);
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
        } else if (item.type == pluginrack::ChainItemType::Multiband || item.type == pluginrack::ChainItemType::DeEsser || item.type == pluginrack::ChainItemType::Limiter) {
            const juce::String name = item.type == pluginrack::ChainItemType::Multiband ? "Multiband Comp" : item.type == pluginrack::ChainItemType::DeEsser ? "De-esser" : "Limiter";
            bool bp = false;
            if (auto* ch = currentChannel()) {
                if (item.type == pluginrack::ChainItemType::Multiband) for (auto& e : ch->multibandEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::DeEsser) for (auto& e : ch->deEsserEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::Limiter) for (auto& e : ch->limiterEffects) if (e.id==item.id) bp=e.bypassed;
            } else if (auto* out = currentOutput()) {
                if (item.type == pluginrack::ChainItemType::Multiband) for (auto& e : out->multibandEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::DeEsser) for (auto& e : out->deEsserEffects) if (e.id==item.id) bp=e.bypassed;
                if (item.type == pluginrack::ChainItemType::Limiter) for (auto& e : out->limiterEffects) if (e.id==item.id) bp=e.bypassed;
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

    // Parked thumbnails: vstThumbs is pruned with its cards (see below) so
    // the native-reparent follow-up can reuse the map as-is.
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
    if (isShowing()) {
        startMiniTimer();
        // The embedded host window is parented to the top-level HWND, not
        // to this view — hiding the Racks tab does not hide it. Re-show it
        // here (and re-hide it below) so it never lingers over the Mixer.
        if (dockedHwnd != 0) showDockedEditor();
    } else {
        stopTimer();
        releaseMiniTap();
        if (dockedHwnd != 0) hideDockedEditor();
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
        for (const auto& output : audioEngine.getOutputs())
            if (output->id == tapArmedId) {
                output->analyzerTap.setMode(AudioEngine::AnalyzerTap::kOff);
                output->eqTap.setMode(AudioEngine::AnalyzerTap::kOff);
                output->dynamicsTap.setMode(AudioEngine::AnalyzerTap::kOff);
                output->multibandTap.setMode(AudioEngine::AnalyzerTap::kOff);
                output->deEsserTap.setMode(AudioEngine::AnalyzerTap::kOff);
                output->limiterTap.setMode(AudioEngine::AnalyzerTap::kOff);
            }
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
    // The docked editor needs its every-tick re-fit whether or not any
    // thumbnail card exists: without the timer the watchdog never polls
    // and a plugin that resizes itself back is never put back.
    const bool hasMinis = eqCard != nullptr || dynamicsCard != nullptr || !vstCards.empty()
                          || dockedHwnd != 0;
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
        // The embedded host window is parented to the top-level HWND, so
        // hiding this view does not hide it — keep it hidden while we are
        // off-screen so it never paints over the Mixer or other tabs.
        if (dockedHwnd != 0) hideDockedEditor();
        return;
    }
    // No in-process editor exists while editors live out-of-process, so no
    // bay layout work runs here. The native-reparent follow-up reinstates it.
    const int fps = juce::jlimit(1, 60, audioEngine.getRackThumbnailFps());
    if (fps != miniTimerHz) {
        miniTimerHz = fps;
        startTimerHz(fps);
    }
    // GitHub method: the bay re-fits on every timer tick (JUCE re-syncs a
    // plugin's native window whenever the plugin resizes itself), so a
    // plugin that moves itself back is put back without a fight. Cheap:
    // layoutDockedEditor writes the frame only when it differs and sends
    // the scale only on a >=1% change. The watchdog below is GitHub's
    // componentMovedOrResized for a window we cannot listen to: it polls
    // the live rect and re-baselines when the plugin insists.
    if (dockedHwnd != 0) layoutDockedEditor();
    watchdogDockedEditor();

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

    // The focused VST's card shows what its editor looks like: capture the
    // embedded host window (SetParent into the bay above) through the
    // existing worker, which keeps the slow PrintWindow repaint off the
    // message thread. A dead host keeps its nameplate plus the badge.
    // Only the focused card captures: one PrintWindow at a time keeps the
    // plugin's repaint work bounded.
    {
        AudioEngine::InsertSlot* focus = nullptr;
        if (focusedInsertId != 0) focus = findInsert(focusedInsertId);
        if (focus == nullptr) {
            if (const auto* inserts = currentInserts(); inserts != nullptr && !inserts->empty()) {
                focus = inserts->front().get();
            }
        }
        void* captureHwnd = nullptr;
       #if JUCE_WINDOWS
        if (focus != nullptr && !focus->pluginHost.isHostDown() && dockedHwnd != 0
            && dockedHost == &focus->pluginHost) {
            auto* hwnd = static_cast<HWND>(
                reinterpret_cast<void*>(static_cast<uintptr_t>(dockedHwnd)));
            if (::IsWindow(hwnd) != FALSE) captureHwnd = hwnd;
        }
       #else
        juce::ignoreUnused(focus);
       #endif
        if (captureHwnd != nullptr && focus != nullptr) {
            const std::uint32_t focusId = focus->id;
            thumbnailWorker.request(captureHwnd, 220,
                                    [this, focusId](juce::Image captured) {
                                        applyVstThumbnail(focusId, captured);
                                    });
        } else {
            thumbnailWorker.clearRequest();
        }
    }
    clearBackgroundPreview();
    clearAllLivePreviews();
}

void RackView::applyVstThumbnail(std::uint32_t insertId, const juce::Image& thumbnail) {
    if (thumbnail.isNull()) return;
    vstThumbs[insertId] = thumbnail;
    if (auto it = vstCards.find(insertId); it != vstCards.end()) {
        if (auto* mini = it->second->miniView()) mini->setVstSnapshot(thumbnail);
    }
}

bool RackView::captureNextBackgroundPreview(int targetWidth) {
    // Out-of-process editors cannot be constructed in-process: background
    // previews stay parked until the bay embeds the host's native window.
    juce::ignoreUnused(targetWidth);
    clearBackgroundPreview();
    return false;
}

void RackView::clearBackgroundPreview() {
    // Parked: background editors are always null while editors live
    // out-of-process. Reset the bookkeeping in case an old pointer lingers.
    backgroundEditor = nullptr;
    backgroundHost = nullptr;
    backgroundInsertId = 0;
}

void RackView::updateAllLivePreviews(int targetWidth) {
    // Out-of-process editors cannot be constructed in-process: all-live
    // previews stay parked until the bay embeds the host's native window.
    juce::ignoreUnused(targetWidth);
    clearAllLivePreviews();
}

void RackView::clearAllLivePreviews() {
    // Parked: live previews are always empty while editors live
    // out-of-process. Reset the bookkeeping in case old entries linger.
    livePreviews.clear();
    livePreviewFailed.clear();
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
        resized();
        return;
    }

    // Out-of-process editors live in the sandboxed host; the bay embeds
    // the host's native window (SetParent) when the host is alive, and
    // shows a status line otherwise. The touch strip stays inert: no
    // in-process instance exists to observe.
    knobWatcher.clear();
    closeDockedEditor();

    dockedHost = &focus->pluginHost;
    if (dockedHost->isHostDown()) {
        // Headless diagnostics (and a host that died between launch and
        // focus): say which. The screenshot path has no hosts at all, so
        // its shot proves the fail-closed line, not a false "not running".
        const bool neverLaunched = !dockedHost->wasEverReady() && !dockedHost->hasHost();
        dockedHost = nullptr;
        noEditorLabel.setText(neverLaunched ? "No sandboxed host in this session - start the app to embed editors"
                                            : "Sandboxed host is not running - slot is silent",
                              juce::dontSendNotification);
        noEditorLabel.setVisible(true);
        noEditorBypass.setVisible(true);
        noEditorBypass.setToggleState(focus->bypassed.get(), juce::dontSendNotification);
        resized();
        return;
    }
    // Ask the host for its editor window and embed it. A 0 HWND means the
    // plugin has no editor (or it failed): keep the status line, fail
    // closed, audio untouched.
    int editorW = 0;
    int editorH = 0;
    const auto hwnd = dockedHost->requestEditorWindow(editorW, editorH);
    // The host reports "fixed-size" when the plugin cannot stretch: the
    // bay centres it at its real size instead of scaling (GH rescaled
    // check, cross-process).
    const bool fixedSize = dockedHost->lastEditorFailReason() == "fixed-size";
    juce::Logger::writeToLog("plugin: editor window for " + focus->pluginName + ": hwnd="
                             + juce::String(hwnd) + " size=" + juce::String(editorW) + "x"
                             + juce::String(editorH)
                             + (fixedSize ? " fixed-size"
                                          : (hwnd == 0 ? " reason="
                                                             + dockedHost->lastEditorFailReason()
                                                       : juce::String())));
    if (hwnd == 0) {
        dockedHost = nullptr;
        noEditorLabel.setText("This plugin has no editor window",
                              juce::dontSendNotification);
        noEditorLabel.setVisible(true);
        noEditorBypass.setVisible(true);
        noEditorBypass.setToggleState(focus->bypassed.get(), juce::dontSendNotification);
        resized();
        return;
    }
    embedDockedHwnd(hwnd, editorW, editorH, fixedSize);
    noEditorLabel.setVisible(false);
    noEditorBypass.setVisible(false);
    resized();
    return;
}

void RackView::embedDockedHwnd(unsigned long long hwndValue, int width, int height) {
    embedDockedHwnd(hwndValue, width, height, false);
}

void RackView::embedDockedHwnd(unsigned long long hwndValue, int width, int height,
                               bool fixedSize) {
   #if JUCE_WINDOWS
    auto* hwnd = static_cast<HWND>(reinterpret_cast<void*>(static_cast<uintptr_t>(hwndValue)));
    if (hwnd == nullptr || ::IsWindow(hwnd) == FALSE) return;
    // Hand the host window to the JUCE holder: it reparents (WS_CHILD),
    // moves/resizes the HWND to follow the holder (DPI included), hides it
    // with the Racks tab, and clips it to the bay like the GH editor.
    dockedHwndView.setHWND(hwnd);
    ::ShowWindow(hwnd, SW_SHOW);
    dockedHwnd = hwndValue;
    // The plugin's own size at 100%: the fit is measured from this, so no
    // layout pass ever feeds back into the size it scales from (GH bay).
    dockedHwndWidth = width > 0 ? width : 400;
    dockedHwndHeight = height > 0 ? height : 300;
    dockedEditorScale = 1.0f;
    // Fixed-size plugin (GH rescaled check): no content scale is in
    // effect, so centre it at its real size instead of scaling.
    dockedEditorFixedSize = fixedSize;
    dockedHwndView.setVisible(true);
    dockedHwndView.toFront(false);
    dockedEditorLayoutPasses = 0;
    dockedEditorSettled = false;
    // The fit loop lives on the timer: make sure it is running even when
    // no thumbnail card exists, or the frame above is never re-asserted.
    startMiniTimer();
    layoutDockedEditor();
    // The host applies the fit scale on its message thread after this
    // returns, and a JUCE setScaleFactor repositions the native window
    // behind our back — so the frame above rarely survives first contact.
    // Re-assert it once the plugin has settled instead of fighting it in
    // a resize loop (GH bay's 400/1200 ms delays).
    juce::Component::SafePointer<RackView> safe(this);
    for (const int delayMs : {400, 1200}) {
        juce::Timer::callAfterDelay(delayMs, [safe] {
            if (safe == nullptr) return;
            safe->dockedEditorLayoutPasses = 0;
            safe->layoutDockedEditor();
        });
    }
   #else
    juce::ignoreUnused(hwndValue, width, height);
   #endif
}

void RackView::releaseDockedHwnd() {
   #if JUCE_WINDOWS
    if (dockedHwnd == 0) return;
    // Return the window to the host's hidden parent: tell the host first
    // so it re-hides the window, then release the holder (which detaches
    // defensively in case the host is already gone). The window stays
    // alive in the host process — never destroyed here.
    if (dockedHost != nullptr) dockedHost->releaseEditorWindow();
    dockedHwndView.setHWND(nullptr);
    dockedHwndView.setVisible(false);
    dockedHwnd = 0;
    dockedHwndWidth = 0;
    dockedHwndHeight = 0;
   #else
    dockedHwndView.setVisible(false);
   #endif
}

void RackView::layoutDockedEditor() {
    // GH method, cross-process edition. Same steps as the GH bay, in
    // holder-local space: fit the natural size into the bay (tighter axis
    // wins, aspect kept), send the scale to the host (which applies it with
    // setScaleFactor exactly like the GH bay does), then centre the scaled
    // visual and clamp it to the bay. The placeholder (dockedHwndView) is
    // positioned like the GH bay positions its editor; positionDockedHwnd
    // moves the native window to cover it with the peer's own area math
    // (DPI included). Sends the scale only on a >=1% change, so the
    // every-tick call from timerCallback stays cheap.
    if (dockedHwnd == 0 || layingOutDockedEditor) return;
    const juce::ScopedValueSetter<bool> guard(layingOutDockedEditor, true);
    const auto bay = editorHolder.getLocalBounds();
    if (bay.isEmpty()) return;
    const int natW = dockedHwndWidth > 0 ? dockedHwndWidth : bay.getWidth();
    const int natH = dockedHwndHeight > 0 ? dockedHwndHeight : bay.getHeight();
    if (dockedEditorNaturalSize.getWidth() != natW
        || dockedEditorNaturalSize.getHeight() != natH) {
        dockedEditorNaturalSize = {natW, natH};
        dockedEditorAppliedSize = {};
    }
    // Whichever of width/height is the tighter constraint decides the
    // scale, so one axis fills the bay and the plugin keeps its aspect.
    // Fixed-size plugins skip the scale entirely (GH rescaled check): the
    // frame is the natural size, centred.
    const float fit = dockedEditorFixedSize
                          ? 1.0f
                          : juce::jmin((float) bay.getWidth() / (float) natW,
                                       (float) bay.getHeight() / (float) natH);
    // What the editor paints now: the host applied our scale to the
    // plugin's natural size (a plugin without content-scale support comes
    // back at its unscaled size — see the host's rescaled check).
    const int scaledW = juce::jmax(1, (int) std::round(natW * dockedEditorScale));
    const int scaledH = juce::jmax(1, (int) std::round(natH * dockedEditorScale));
    const auto fitted = bay.withSizeKeepingCentre(
        juce::jmax(1, juce::jmin(scaledW, bay.getWidth())),
        juce::jmax(1, juce::jmin(scaledH, bay.getHeight())));
    // A new bay size re-arms the settle so the plugin can fill the new bay
    // instead of staying small.
    if (dockedBayW != bay.getWidth() || dockedBayH != bay.getHeight()) {
        dockedBayW = bay.getWidth();
        dockedBayH = bay.getHeight();
        dockedEditorSettled = false;
    }
    const bool wantChanged = (dockedEditorWant != fitted);
    dockedEditorWant = fitted;
    // Ignore sub-percent moves: dragging the window would otherwise re-lay
    // out the plugin UI on every pixel.
    if (fit > 0.0f && std::abs(fit - dockedEditorScale) >= 0.01f) {
        // The scale request is ours: the host's resize echo must not be
        // read as the plugin choosing a size of its own (see the
        // watchdog). Mirrors the GH bay's echo guard around setScaleFactor.
        // The guard must outlive this call: the host applies the scale on
        // its own message thread, so the echo arrives ticks later.
        applyingDockedEditorScale = true;
        dockedEditorScale = fit;
        dockedEditorAppliedSize = {fitted.getWidth(), fitted.getHeight()};
        if (dockedHost != nullptr) dockedHost->sendEditorScale(fit);
        juce::Logger::writeToLog("plugin: editor fit: natural=" + juce::String(natW) + "x"
                                 + juce::String(natH) + " bay=" + juce::String(bay.getWidth())
                                 + "x" + juce::String(bay.getHeight()) + " scale="
                                 + juce::String(fit, 2) + " frame=" + fitted.toString());
    } else if (wantChanged) {
        juce::Logger::writeToLog("plugin: editor frame: " + fitted.toString() + " in bay "
                                 + bay.toString());
    }
    // Position the placeholder like the GH bay positions its editor;
    // the native window follows it (see positionDockedHwnd).
    if (dockedHwndView.getBounds() != fitted) dockedHwndView.setBounds(fitted);
    positionDockedHwnd();
}

void RackView::positionDockedHwnd() {
    // The HWND holder moves the native window to follow itself; re-assert
    // the bounds so a host-side resize behind our back is put back.
   #if JUCE_WINDOWS
    if (dockedHwnd == 0) return;
    dockedHwndView.updateHWNDBounds();
   #endif
}

void RackView::applyDockedEditorFit(const juce::Rectangle<int>& bay) {
    // Parked: see layoutDockedEditor.
    juce::ignoreUnused(bay);
}

void RackView::watchdogDockedEditor() {
    // GH componentMovedOrResized, cross-process edition. The host
    // cannot notify us when the plugin resizes itself, so poll the live
    // window a few times a second: when it drifted from the want frame,
    // treat it as the plugin picking a new UI size — move the baseline the
    // fit is measured from (like GH's natural-size re-baseline), then
    // re-fit and centre. Settles after a few passes so a plugin that
    // insists on its own size ends centred at its real size instead of
    // fighting the bay forever. A drift that arrives while the scale echo
    // guard is set is ours, never the plugin's own choice. Compared in
    // physical pixels via the peer's own scale (see positionDockedHwnd);
    // re-baselined back to logical.
   #if JUCE_WINDOWS
    if (dockedHwnd == 0 || dockedEditorSettled) return;
    if (dockedHwndView.getHWND() == nullptr) return;
    auto* hwnd = static_cast<HWND>(reinterpret_cast<void*>(static_cast<uintptr_t>(dockedHwnd)));
    if (::IsWindow(hwnd) == FALSE) {
        dockedHwnd = 0;
        return;
    }
    if (++dockedWatchdogTick < 10) return;
    dockedWatchdogTick = 0;
    auto* peer = dockedHwndView.getPeer();
    if (peer == nullptr) return;
    auto* topHwnd = static_cast<HWND>(peer->getNativeHandle());
    if (topHwnd == nullptr || ::IsWindow(topHwnd) == FALSE) return;
    const float platformScale = (float) peer->getPlatformScaleFactor();
    const auto wantPhysical = (dockedEditorWant.toFloat() * platformScale).getSmallestIntegerContainer();
    POINT wantOrigin = {wantPhysical.getX(), wantPhysical.getY()};
    ::ClientToScreen(topHwnd, &wantOrigin);
    RECT rect = {};
    if (::GetWindowRect(hwnd, &rect) == FALSE) return;
    const int liveW = rect.right - rect.left;
    const int liveH = rect.bottom - rect.top;
    const int driftX = std::abs(rect.left - wantOrigin.x);
    const int driftY = std::abs(rect.top - wantOrigin.y);
    const int driftW = std::abs(liveW - wantPhysical.getWidth());
    const int driftH = std::abs(liveH - wantPhysical.getHeight());
    if (driftX <= 2 && driftY <= 2 && driftW <= 2 && driftH <= 2) {
        dockedEditorSettled = true;
        // The host caught up with our fit: the echo is over, so a later
        // drift is the plugin's own choice again.
        applyingDockedEditorScale = false;
        return;
    }
    // Clamp first, ask questions later: the live window must never paint
    // outside the want frame (the spill-over-the-bay report). The holder
    // re-asserts the frame on the next layout; this caps the native window
    // right now so no frame is ever shown overflowing.
    if (liveW > wantPhysical.getWidth() + 2 || liveH > wantPhysical.getHeight() + 2) {
        ::SetWindowPos(hwnd, nullptr, 0, 0, wantPhysical.getWidth(), wantPhysical.getHeight(),
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    // The scale echo: the host is still applying our fit, so the live
    // window has not caught up yet — not the plugin choosing a size.
    // Re-assert the frame (which the host's own resize may have moved)
    // without counting a pass or touching the natural-size baseline.
    // Before that, ask the host for the editor's CURRENT outer size: when
    // it already matches the want frame in logical units, the native
    // window is just behind (DPI lag) — clamp it and wait, never count a
    // drift pass. Only a host size that actually differs means the plugin
    // picked a new size of its own.
    if (applyingDockedEditorScale) {
        if (dockedHost != nullptr) {
            int hostW = 0;
            int hostH = 0;
            if (dockedHost->requestEditorSize(hostW, hostH) && hostW > 0 && hostH > 0) {
                const int wantW = dockedEditorWant.getWidth();
                const int wantH = dockedEditorWant.getHeight();
                if (std::abs(hostW - wantW) <= 2 && std::abs(hostH - wantH) <= 2) {
                    layoutDockedEditor();
                    return;
                }
            }
        }
        layoutDockedEditor();
        return;
    }
    if (++dockedEditorLayoutPasses > 6) {
        // The plugin insists on its own size (fixed-size UI, or no
        // content-scale support): GH centres it at its real size. Divide
        // out our own fit first — the live window already carries our
        // scale, so the baseline must be the UNSCALED size, or the next
        // fit compounds on top of itself (the 1372x705 blowup).
        const float inv = platformScale > 0.0f ? 1.0f / platformScale : 1.0f;
        const float unfit = dockedEditorScale > 0.0f ? 1.0f / dockedEditorScale : 1.0f;
        dockedHwndWidth = juce::jmax(1, (int) std::round(liveW * inv * unfit));
        dockedHwndHeight = juce::jmax(1, (int) std::round(liveH * inv * unfit));
        dockedEditorNaturalSize = {};
        dockedEditorAppliedSize = {};
        dockedEditorScale = 1.0f;
        applyingDockedEditorScale = false;
        dockedEditorSettled = true;
        juce::Logger::writeToLog("plugin: editor keeps its own size " + juce::String(liveW)
                                 + "x" + juce::String(liveH) + " physical ("
                                 + juce::String(dockedHwndWidth) + "x"
                                 + juce::String(dockedHwndHeight) + " logical), centring");
        layoutDockedEditor();
        return;
    }
    juce::Logger::writeToLog("plugin: editor drifted to " + juce::String(rect.left) + ","
                             + juce::String(rect.top) + " " + juce::String(liveW) + "x"
                             + juce::String(liveH) + ", re-asserting fit (pass "
                             + juce::String(dockedEditorLayoutPasses) + ")");
    layoutDockedEditor();
   #endif
}

juce::Rectangle<int> RackView::dockedEditorVisualBounds() const {
    return {};
}

void RackView::releaseOrphanedDockedEditor() {
    if (dockedHost != nullptr && dockedHostAlive()) return;
    closeDockedEditor();
}

void RackView::closeDockedEditor() {
    // Release the embedded host window back to its owner before dropping
    // the bay state: the window stays alive in the host process, only the
    // parenting ends here. Then drop the watcher and any parked preview
    // state and release the bay.
    releaseDockedHwnd();
    knobWatcher.clear();
    thumbnailWorker.clearRequest();
    clearBackgroundPreview();
    clearAllLivePreviews();
    dockedEditor = nullptr;
    dockedHost = nullptr;
    dockedEditorNaturalSize = {};
    dockedEditorAppliedSize = {};
    dockedEditorWant = {};
    dockedBayW = 0;
    dockedBayH = 0;
    dockedEditorSettled = false;
    dockedWatchdogTick = 0;
    dockedEditorScale = 1.0f;
    dockedEditorFixedSize = false;
    applyingDockedEditorScale = false;
    dockedEditorLayoutPasses = 0;
    layingOutDockedEditor = false;
}

void RackView::hideDockedEditor() {
    // A panel owns the bay: hide the embedded window (keep the holder so
    // it comes back on showDockedEditor) and stop thumbnails. Hiding the
    // holder hides the HWND with it.
    thumbnailWorker.clearRequest();
    dockedHwndView.setVisible(false);
}

// Brings the insert editor back once no panel owns the bay any more.
void RackView::showDockedEditor() {
    if (dockedHwnd == 0) {
        // No window (or it died with its host): re-run the dock path, which
        // re-requests from the host or shows the status line fail-closed.
        refreshEditor();
        return;
    }
    if (dockedHwndView.isVisible() == false) dockedHwndView.setVisible(true);
   #if JUCE_WINDOWS
    if (dockedHwndView.getHWND() != nullptr) {
        dockedEditorSettled = false;
        dockedWatchdogTick = 0;
        layoutDockedEditor();
        return;
    }
    dockedHwnd = 0;
   #endif
    // No window (or it died with its host): re-run the dock path, which
    // re-requests from the host or shows the status line fail-closed.
    refreshEditor();
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
    if (focusedEffectWindow == nullptr) showDockedEditor();
}

void RackView::closeEffectWindows() {
    if (focusedEffectWindow == nullptr) return;
    auto* doomed = focusedEffectWindow;
    focusedEffectWindow = nullptr;
    multibandWindow = nullptr;
    deEsserWindow = nullptr;
    limiterWindow = nullptr;
    eqFocused = false;
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
            for (auto& e : effects) if (e.id == effect.id) {
                const bool bp = e.bypassed; e = effect;
                if (!bypassChanged) e.bypassed = bp;
            }
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
            for (auto& e : effects) if (e.id == effect.id) {
                const bool bp = e.bypassed; e = effect;
                if (!bypassChanged) e.bypassed = bp;
            }
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
            for (auto& e : effects) if (e.id == effect.id) {
                const bool bp = e.bypassed; e = effect;
                if (!bypassChanged) e.bypassed = bp;
            }
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

void RackView::refreshEqButton() {
    // No standalone button left: the insert cards carry the bypass state.
    // Kept so callers do not need to change; the card refresh happens in
    // refreshSlots() and the timer tick.
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
                                      const std::vector<float>& channelHues) {
    if (names == channelNames && devices == deviceNames && hues == channelHues) return;
    names = std::move(channelNames);
    devices = std::move(deviceNames);
    hues = channelHues;
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
    std::vector<float> sourceHues;
    if (const auto* out = currentOutput()) {
        for (const auto& channel : audioEngine.getChannels()) {
            if (audioEngine.getSendGain(channel->id, out->id) <= -30.0f) continue;
            sourceNames.add(channel->name);
            sourceHues.push_back(channel->colourHue);
            const auto device = audioEngine.getChannelDeviceState(channel->id).deviceName;
            sourceDevices.add(device.isNotEmpty() ? device : juce::String("No input device"));
        }
    }
    sourceList.setRows(std::move(sourceNames), std::move(sourceDevices),
                       std::move(sourceHues));
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
    clearBackgroundPreview();
    clearAllLivePreviews();
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
    refreshSlots();

    // Keep embedded editors inside the framed center bay. The viewport and
    // its child editors paint opaque surfaces, so using the panel bounds
    // directly would cover the panel outline.
    const auto editorArea = centerArea.reduced(2);
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
    if (focusedEffectWindow == nullptr) {
        editorHolder.setSize(juce::jmax(0, vw), juce::jmax(0, vh));
        // An embedded host window tracks the holder: re-fit it on every
        // layout so resizes move it with the bay.
        layoutDockedEditor();
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
            const auto& outs = safe->audioEngine.getOutputs();
            for (int i = 0; i < static_cast<int>(outs.size()); ++i) {
                if (outs[static_cast<std::size_t>(i)]->id
                    == static_cast<std::uint32_t>(selected)) {
                    safe->audioEngine.setOutputColourHue(i, newHue);
                    break;
                }
            }
        }
        safe->rebuild();
    });
}

juce::String RackView::insertPlugin(const juce::File& file, int pluginUniqueId) {
    // The sandboxed host starts on the message thread, and some plugins spend
    // seconds initialising inside it (Native Instruments' Raum needs ~3.5 s).
    // Show a wait cursor now so the window explains the stall instead of just
    // freezing.
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
    // The bay status always describes the selected chain, which is the
    // chain this erase targets: release it before the erase.
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
    } else if (item.type == pluginrack::ChainItemType::Multiband) {
        if (selectedIsOutput) {
            if (auto* output = currentOutput()) {
                auto effects = output->multibandEffects;
                for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
                audioEngine.setOutputMultiband(output->id, effects);
            }
        } else if (auto* channel = currentChannel()) {
            auto effects = channel->multibandEffects;
            for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
            audioEngine.setChannelMultiband(channel->id, effects);
        }
        if (multibandWindow != nullptr) multibandWindow->setBypassed(bypassed);
    } else if (item.type == pluginrack::ChainItemType::DeEsser) {
        if (selectedIsOutput) {
            if (auto* output = currentOutput()) {
                auto effects = output->deEsserEffects;
                for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
                audioEngine.setOutputDeEssers(output->id, effects);
            }
        } else if (auto* channel = currentChannel()) {
            auto effects = channel->deEsserEffects;
            for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
            audioEngine.setChannelDeEssers(channel->id, effects);
        }
        if (deEsserWindow != nullptr) deEsserWindow->setBypassed(bypassed);
    } else if (item.type == pluginrack::ChainItemType::Limiter) {
        if (selectedIsOutput) {
            if (auto* output = currentOutput()) {
                auto effects = output->limiterEffects;
                for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
                audioEngine.setOutputLimiters(output->id, effects);
            }
        } else if (auto* channel = currentChannel()) {
            auto effects = channel->limiterEffects;
            for (auto& e : effects) if (e.id == item.id) e.bypassed = bypassed;
            audioEngine.setChannelLimiters(channel->id, effects);
        }
        if (limiterWindow != nullptr) limiterWindow->setBypassed(bypassed);
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
    else if (item.type == pluginrack::ChainItemType::Dynamics) focusDynamics(pluginrack::DynamicsEffectType::Compressor);
    else if (item.type == pluginrack::ChainItemType::Multiband) focusMultiband();
    else if (item.type == pluginrack::ChainItemType::DeEsser) focusDeEsser();
    else if (item.type == pluginrack::ChainItemType::Limiter) focusLimiter();
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
        }
    } else if (auto* channel = currentChannel()) {
        if (item.type == pluginrack::ChainItemType::Multiband && std::none_of(channel->multibandEffects.begin(), channel->multibandEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = channel->multibandEffects; fx.push_back(pluginrack::makeDefaultMultibandEffect(item.id)); audioEngine.setChannelMultiband(channel->id, fx); }
        if (item.type == pluginrack::ChainItemType::DeEsser && std::none_of(channel->deEsserEffects.begin(), channel->deEsserEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = channel->deEsserEffects; fx.push_back(pluginrack::makeDefaultDeEsserEffect(item.id)); audioEngine.setChannelDeEssers(channel->id, fx); }
        if (item.type == pluginrack::ChainItemType::Limiter && std::none_of(channel->limiterEffects.begin(), channel->limiterEffects.end(), [&item](const auto& e){return e.id==item.id;})) { auto fx = channel->limiterEffects; fx.push_back(pluginrack::makeDefaultLimiterEffect(item.id)); audioEngine.setChannelLimiters(channel->id, fx); }
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
    clearBackgroundPreview();
    clearAllLivePreviews();
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
    // and the VST status never appears.
    closeEffectWindows();
    closeEqWindow();
    closeDynamicsWindow();
    focusedInsertId = insert->id;
    refreshEditor();
    return true;
}
