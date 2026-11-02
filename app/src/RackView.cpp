#include "RackView.h"

#include <algorithm>

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
        rebuild();
    };
    addAndMakeVisible(channelCombo);
    channelCombo.setColour(juce::ComboBox::backgroundColourId, theme::control());
    channelCombo.setColour(juce::ComboBox::textColourId, theme::text());
    channelCombo.setColour(juce::ComboBox::arrowColourId, theme::dim());
    channelCombo.setColour(juce::ComboBox::outlineColourId, theme::border());

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

    emptyLabel.setText("No chains yet - add a channel or output in the Mixer view.",
                       juce::dontSendNotification);
    emptyLabel.setJustificationType(juce::Justification::centred);
    emptyLabel.setFont(theme::ui::bodyFont());
    emptyLabel.setColour(juce::Label::textColourId, theme::dim());
    emptyLabel.setVisible(false);
    addAndMakeVisible(emptyLabel);

    tapLeft.assign(static_cast<std::size_t>(MiniSpectrum::kFftSize), 0.0f);
    tapRight.assign(static_cast<std::size_t>(MiniSpectrum::kFftSize), 0.0f);

    rebuild();
}

RackView::~RackView() {
    stopTimer();
    releaseMiniTap();
    closeEqWindow();
    closeDynamicsWindow();
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
        juce::String sub = ch->inputDeviceName.isNotEmpty() ? ch->inputDeviceName : juce::String("No input");
        sub += ch->layout == pluginrack::ChannelLayout::Mono ? " - Mono" : " - Stereo";
        subLabel.setText(sub, juce::dontSendNotification);
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
        meter = std::make_unique<LevelMeter>(ch->peakL, ch->peakR);
    } else if (out != nullptr) {
        nameLabel.setText(out->name, juce::dontSendNotification);
        subLabel.setText(out->outputDeviceName.isNotEmpty() ? out->outputDeviceName
                                                            : juce::String("No device"),
                         juce::dontSendNotification);
        gainSlider.setValue(out->gainDb.get(), juce::dontSendNotification);
        muteButton.setToggleState(out->muted.get(), juce::dontSendNotification);
        soloButton.setVisible(false);
        panCaption.setVisible(false);
        panSlider.setVisible(false);
        trimCaption.setVisible(false);
        trimKnob.setVisible(false);
        meter = std::make_unique<LevelMeter>(out->peakL, out->peakR);
    }
    if (meter != nullptr) addAndMakeVisible(*meter);
    refreshEqButton();
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

void RackView::refreshSlots() {
    slots.clear();
    eqCard = nullptr;
    dynamicsCard = nullptr;
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

    bool dynamicsPlaced = false;
    for (const auto& item : items) {
        if (item.type == pluginrack::ChainItemType::Eq) {
            auto* slot = slots.add(new InsertSlotComponent(*this, row));
            slot->setBuiltIn(item, "EQ",
                             selectedIsOutput ? currentOutput()->eqBypassed.get()
                                              : currentChannel()->eqBypassed.get());
            auto mini = std::make_unique<InsertMiniView>();
            mini->showEq(selectedIsOutput ? currentOutput()->eqBands : currentChannel()->eqBands,
                         liveRateHz);
            slot->setMiniView(std::move(mini));
            slot->setSelected(eqFocused);
            eqCard = slot;
            place(slot);
        } else if (item.type == pluginrack::ChainItemType::Dynamics) {
            // Compressor and gate are two chain rows drawn as one card: the
            // first row seen places it, the other is its partner.
            if (dynamicsPlaced) continue;
            dynamicsPlaced = true;
            pluginrack::ChainItem partner{pluginrack::ChainItemType::Dynamics, 0};
            for (const auto& other : items)
                if (other.type == pluginrack::ChainItemType::Dynamics && other.id != item.id)
                    partner = other;

            const auto* firstEffect = dynamicsEffectFor(item.id);
            const auto* partnerEffect = partner.id != 0 ? dynamicsEffectFor(partner.id) : nullptr;
            const auto* compressor =
                firstEffect != nullptr
                        && firstEffect->type == pluginrack::DynamicsEffectType::Compressor
                    ? firstEffect
                    : (partnerEffect != nullptr
                               && partnerEffect->type == pluginrack::DynamicsEffectType::Compressor
                           ? partnerEffect
                           : nullptr);
            const auto* gate = firstEffect != nullptr
                                       && firstEffect->type == pluginrack::DynamicsEffectType::Gate
                                   ? firstEffect
                                   : (partnerEffect != nullptr
                                              && partnerEffect->type
                                                     == pluginrack::DynamicsEffectType::Gate
                                          ? partnerEffect
                                          : nullptr);

            auto* slot = slots.add(new InsertSlotComponent(*this, row));
            if (compressor != nullptr && gate != nullptr) {
                // Each switch has to address the chain row that carries it.
                const auto compressorItem = compressor == firstEffect ? item : partner;
                const auto gateItem = gate == firstEffect ? item : partner;
                slot->setBuiltInPair("Gate/Comp", compressorItem, compressor->bypassed,
                                     gateItem, gate->bypassed);
                auto mini = std::make_unique<InsertMiniView>();
                mini->showDynamicsPair(*compressor, *gate);
                slot->setMiniView(std::move(mini));
                dynamicsCard = slot;
            } else {
                const auto* single = compressor != nullptr ? compressor : gate;
                slot->setBuiltIn(item, compressor != nullptr ? "Compressor" : "Gate",
                                 single != nullptr && single->bypassed);
                if (single != nullptr) {
                    auto mini = std::make_unique<InsertMiniView>();
                    mini->showDynamics(*single);
                    slot->setMiniView(std::move(mini));
                    dynamicsCard = slot;
                }
            }
            slot->setSelected(dynamicsFocused);
            place(slot);
        } else if (inserts != nullptr) {
            auto it = std::find_if(inserts->begin(), inserts->end(),
                                   [id = item.id](const auto& insert) { return insert->id == id; });
            if (it != inserts->end()) {
                auto* slot = slots.add(new InsertSlotComponent(*this, row));
                slot->setInsert(it->get());
                auto mini = std::make_unique<InsertMiniView>();
                mini->showVst();
                if (const auto thumb = vstThumbs.find(it->get()->id); thumb != vstThumbs.end())
                    mini->setVstSnapshot(thumb->second);
                slot->setMiniView(std::move(mini));
                slot->setSelected(dockedHost != nullptr && focusedInsertId == it->get()->id);
                vstCards[it->get()->id] = slot;
                place(slot);
            }
        }
    }
    chainRowCount = row;

    // Pad the list out to the fixed 8-slot chain; the spare VST rows are the
    // add control, so the insert list never changes length while you work. A
    // chain already holding its five VSTs gets no add row: the engine refuses
    // a sixth insert, and the merged Gate/Comp card leaves one row spare here,
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
    const int fps = juce::jlimit(1, 60, audioEngine.getRackThumbnailFps());
    if (fps != miniTimerHz) {
        miniTimerHz = fps;
        startTimerHz(fps);
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
    if (eqCard != nullptr) {
        if (auto* channel = currentChannel()) {
            if (auto* mini = eqCard->miniView()) {
                mini->setEqBands(channel->eqBands, liveRateHz);
                mini->setSpectrumOffsetDb(channel->eqRtaOffsetDb);
                mini->setWeighting(channel->eqRtaWeighting);
            }
            eqCard->setBuiltInBypassed(channel->eqBypassed.get());
        } else if (auto* output = currentOutput()) {
            if (auto* mini = eqCard->miniView()) {
                mini->setEqBands(output->eqBands, liveRateHz);
                mini->setSpectrumOffsetDb(output->eqRtaOffsetDb);
                mini->setWeighting(output->eqRtaWeighting);
            }
            eqCard->setBuiltInBypassed(output->eqBypassed.get());
        }
    }

    if (tap != nullptr && eqCard != nullptr) {
        tap->snapshot(true, tapLeft.data(), tapRight.data(), MiniSpectrum::kFftSize);
        miniSpectrum.refresh(tapLeft.data(), tapRight.data());
        if (auto* mini = eqCard->miniView())
            mini->setSpectrum(miniSpectrum.magnitudes(), miniSpectrum.numBins());
    }

    if (tap != nullptr && dynamicsCard != nullptr) {
        tap->snapshot(false, tapLeft.data(), tapRight.data(), kLevelFrames);
        const auto* compressor = dynamicsEffectFor(1);
        const auto* gate = dynamicsEffectFor(2);
        // The card is one block, so it reports one combined reduction, the
        // same sum the editor's gauge shows.
        float levelDb = -60.0f;
        float combinedGainDb = 0.0f;
        if (compressor != nullptr) {
            compressorMeter.update(tapLeft.data(), tapRight.data(), kLevelFrames, *compressor,
                                   static_cast<double>(fps));
            levelDb = compressorMeter.inputDb;
            combinedGainDb += compressorMeter.gainDb;
        }
        if (gate != nullptr) {
            gateMeter.update(tapLeft.data(), tapRight.data(), kLevelFrames, *gate,
                             static_cast<double>(fps));
            if (compressor == nullptr) levelDb = gateMeter.inputDb;
            combinedGainDb += gateMeter.gainDb;
        }

        if (compressor != nullptr && gate != nullptr) {
            dynamicsCard->setPairBypassed(compressor->bypassed, gate->bypassed);
        } else if (const auto* single = compressor != nullptr ? compressor : gate) {
            dynamicsCard->setBuiltInBypassed(single->bypassed);
        }

        if (auto* mini = dynamicsCard->miniView()) {
            if (compressor != nullptr && gate != nullptr) mini->setDynamicsPair(*compressor, *gate);
            else if (compressor != nullptr) mini->setDynamics(*compressor);
            else if (gate != nullptr) mini->setDynamics(*gate);
            if (compressor != nullptr || gate != nullptr)
                mini->setLiveGain(levelDb, combinedGainDb);
        }
    }

    // The focused VST's card shows what its editor looks like. JUCE hands out
    // one editor per processor, so a live thumbnail and the bay cannot
    // coexist; and a hosted VST3 editor paints itself into its own window, so
    // it is captured from that window when it has one.
    if (dockedEditor != nullptr && focusedInsertId != 0 && !eqFocused && !dynamicsFocused) {
        const int targetWidth = juce::jmax(32, slotViewport.getWidth() - 2 * kSlotInset);
        juce::Image thumb;
        if (auto* pluginWindow = nativedisplay::pluginWindowOf(*dockedEditor)) {
            // A hosted VST3 editor is its own native window whose JUCE paint is
            // a black fill, so only a window capture sees the plugin's pixels.
            thumb = nativedisplay::captureWindow(pluginWindow, targetWidth);
        } else {
            const auto editorBounds = dockedEditor->getLocalBounds();
            if (!editorBounds.isEmpty()) {
                const float scale = juce::jlimit(0.02f, 1.0f,
                                                 static_cast<float>(targetWidth)
                                                     / static_cast<float>(editorBounds.getWidth()));
                thumb = dockedEditor->createComponentSnapshot(editorBounds, true, scale);
            }
        }
        if (!thumb.isNull()) {
            vstThumbs[focusedInsertId] = thumb;
            if (const auto card = vstCards.find(focusedInsertId); card != vstCards.end())
                if (auto* mini = card->second->miniView()) mini->setVstSnapshot(thumb);
        }
    }
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

    // Already docked for this insert: reuse the live editor (it may only be
    // hidden behind an EQ/dynamics panel) instead of rebuilding the VST UI.
    if (dockedEditor != nullptr && dockedHost == &focus->pluginHost) {
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
        if (dockedEditor->getParentComponent() != &editorHolder)
            editorHolder.addAndMakeVisible(dockedEditor);
        dockedEditor->setVisible(true);
        resized();
        return;
    }
    closeDockedEditor();

    dockedHost = &focus->pluginHost;
    dockedHost->closeFloatingEditor();
    dockedEditor = dockedHost->createEditorForDock();
    if (dockedEditor == nullptr) {
        dockedHost = nullptr;
        noEditorLabel.setText("No GUI - generic sliders", juce::dontSendNotification);
        noEditorLabel.setVisible(true);
        noEditorBypass.setVisible(true);
        noEditorBypass.setToggleState(focus->bypassed.get(), juce::dontSendNotification);
        resized();
        return;
    }

    // Whatever the plugin sized itself to before any layout pass; reused for
    // every later pass so the editor is never stretched by the bay.
    dockedEditorSize = dockedEditor->getBounds();

    noEditorLabel.setVisible(false);
    noEditorBypass.setVisible(false);
    editorHolder.addAndMakeVisible(dockedEditor);
    resized();
}

void RackView::closeDockedEditor() {
    if (dockedEditor == nullptr) {
        dockedHost = nullptr;
        return;
    }
    // A floating window may have taken ownership of the editor (reparented
    // away from the dock): the window owns it now, so release our pointers
    // without deleting.
    if (dockedEditor->getParentComponent() != nullptr
        && dockedEditor->getParentComponent() != &editorHolder) {
        dockedEditor = nullptr;
        dockedHost = nullptr;
        dockedEditorSize = {};
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
    dockedEditorSize = {};
}

void RackView::hideDockedEditor() {
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

void RackView::refreshOutputButtons() {
    outputButtons.clear();
    const auto* ch = currentChannel();
    const auto& outputs = audioEngine.getOutputs();
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
}

void RackView::rebuild() {
    ensureSelection();
    const bool hasChain = selectedId >= 0;
    emptyLabel.setVisible(!hasChain);

    nameLabel.setVisible(hasChain);
    subLabel.setVisible(hasChain);
    gainCaption.setVisible(hasChain);
    gainSlider.setVisible(hasChain);
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
    const int gap = 8;

    auto leftArea = area.removeFromLeft(leftW);
    area.removeFromLeft(gap);
    auto rightArea = area.removeFromRight(rightW);
    area.removeFromRight(gap);
    auto pluginArea = area.removeFromLeft(pluginW);
    area.removeFromLeft(gap);
    auto centerArea = area;

    // left column
    auto la = leftArea.reduced(4);
    la.removeFromTop(14);
    channelCombo.setBounds(la.removeFromTop(24));
    la.removeFromTop(4);
    nameLabel.setBounds(la.removeFromTop(20));
    subLabel.setBounds(la.removeFromTop(16));
    la.removeFromTop(10);

    trimCaption.setBounds(la.removeFromTop(14));
    auto trimArea = la.removeFromTop(56);
    trimKnob.setBounds(trimArea.withSizeKeepingCentre(52, 52));
    la.removeFromTop(6);

    la.removeFromBottom(6); // breathing room above the panel edge
    auto btnRow = la.removeFromBottom(24);
    la.removeFromBottom(14);

    auto panArea = la.removeFromBottom(92);
    panCaption.setBounds(panArea.removeFromTop(14));
    panSlider.setBounds(panArea.withSizeKeepingCentre(64, 64));
    la.removeFromBottom(10);

    auto faderRow = la;
    auto meterColumn = faderRow.removeFromRight(24);
    faderRow.removeFromRight(4);
    gainCaption.setBounds(faderRow.removeFromTop(14));
    gainSlider.setBounds(faderRow.reduced(0, 4));
    // The meter spans the fader's track, not the fader component, so its bars
    // start and end where the fader's travel does.
    if (meter != nullptr)
        meter->setBounds(LevelMeter::faderTrackBounds(gainSlider)
                             .withX(meterColumn.getX())
                             .withWidth(meterColumn.getWidth()));

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
        // Keep the size the plugin declared; only the empty fallback tracks it.
        const auto preferred = dockedEditorSize.isEmpty() ? dockedEditor->getBounds()
                                                          : dockedEditorSize;
        const int ew = juce::jmax(200, preferred.getWidth());
        const int eh = juce::jmax(150, preferred.getHeight());
        editorHolder.setSize(juce::jmax(vw, ew), juce::jmax(vh, eh));
        dockedEditor->setBounds(editorHolder.getLocalBounds().withSizeKeepingCentre(ew, eh));
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
    const int gap = 8;
    auto leftArea = area.removeFromLeft(leftW);
    area.removeFromLeft(gap);
    auto rightArea = area.removeFromRight(rightW);
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

    theme::ui::drawSectionCaption(g, { leftArea.getX() + 8, leftArea.getY() + 4, 70, 12 }, "INPUT");
    theme::ui::drawSectionCaption(g, { pluginArea.getX() + 8, pluginArea.getY() + 4, 80, 12 }, "INSERTS");
    theme::ui::drawSectionCaption(g, { centerArea.getX() + 10, centerArea.getY() + 6, 110, 12 },
                                  "PLUGIN EDITOR");
    theme::ui::drawSectionCaption(g, { rightArea.getX() + 8, rightArea.getY() + 4, 70, 12 }, "OUTPUT");
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

juce::String RackView::insertPlugin(const juce::File& file) {
    if (auto* ch = currentChannel()) {
        const auto error = audioEngine.addPlugin(ch->id, file);
        refreshAfterInsertChange();
        return error;
    }
    if (auto* out = currentOutput()) {
        const auto error = audioEngine.addOutputPlugin(out->id, file);
        refreshAfterInsertChange();
        return error;
    }
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

int RackView::chainIndexForRow(int row) {
    const auto* chain = selectedIsOutput
                            ? (currentOutput() == nullptr ? nullptr : &currentOutput()->chain)
                            : (currentChannel() == nullptr ? nullptr : &currentChannel()->chain);
    if (chain == nullptr) return row;
    int visible = 0;
    bool pairSeen = false;
    for (int index = 0; index < static_cast<int>(chain->size()); ++index) {
        const auto& entry = (*chain)[static_cast<std::size_t>(index)];
        if (entry.type == pluginrack::ChainItemType::Dynamics) {
            // The pair's second row has no row of its own, and a drop between
            // the two would split a single card, so it is never a landing spot.
            if (pairSeen) continue;
            pairSeen = true;
        }
        if (visible == row) return index;
        ++visible;
    }
    return static_cast<int>(chain->size());
}

void RackView::moveChainItemRaw(pluginrack::ChainItem item, int chainIndex) {
    if (selectedIsOutput)
        audioEngine.moveOutputChainItem(selectedId, item, chainIndex);
    else
        audioEngine.moveChannelChainItem(selectedId, item, chainIndex);
}

void RackView::moveDynamicsPair(pluginrack::ChainItem item, int targetRowIndex) {
    const auto* chain = selectedIsOutput
                            ? (currentOutput() == nullptr ? nullptr : &currentOutput()->chain)
                            : (currentChannel() == nullptr ? nullptr : &currentChannel()->chain);
    if (chain == nullptr) return;

    pluginrack::ChainItem partner{pluginrack::ChainItemType::Dynamics, 0};
    for (const auto& entry : *chain)
        if (entry.type == pluginrack::ChainItemType::Dynamics && entry.id != item.id)
            partner = entry;

    moveChainItemRaw(item, chainIndexForRow(targetRowIndex));
    if (partner.id == 0) return;

    // The card stands for both rows, so the partner follows it. Removing it
    // first shifts the anchor down when it sat before it.
    const auto indexOf = [chain](const pluginrack::ChainItem& needle) {
        for (int i = 0; i < static_cast<int>(chain->size()); ++i)
            if ((*chain)[static_cast<std::size_t>(i)] == needle) return i;
        return -1;
    };
    const int anchorIndex = indexOf(item);
    const int partnerIndex = indexOf(partner);
    if (anchorIndex < 0 || partnerIndex < 0) return;
    moveChainItemRaw(partner, partnerIndex < anchorIndex ? anchorIndex : anchorIndex + 1);
}

void RackView::reorderChainItem(pluginrack::ChainItem item, int targetIndex) {
    if (item.type == pluginrack::ChainItemType::Dynamics)
        moveDynamicsPair(item, targetIndex);
    else
        moveChainItemRaw(item, chainIndexForRow(targetIndex));
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

void RackView::openPicker() {
    fileChooser = std::make_unique<juce::FileChooser>(
        "Select a VST3 plugin", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.vst3");
    juce::Component::SafePointer<RackView> safeThis(this);
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