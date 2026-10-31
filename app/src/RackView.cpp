#include "RackView.h"

namespace {
constexpr int kSlotRowHeight = 36;
constexpr int kSlotGap = 4;
constexpr int kAddButtonWidth = 120;
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

    trimCaption.setText("Gain", juce::dontSendNotification);
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

    panSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
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

    insertsHeader.setText("Inserts", juce::dontSendNotification);
    insertsHeader.setJustificationType(juce::Justification::centredLeft);
    insertsHeader.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    insertsHeader.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(insertsHeader);

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

    emptySlotsLabel.setJustificationType(juce::Justification::centredLeft);
    emptySlotsLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    emptySlotsLabel.setColour(juce::Label::textColourId, theme::dim());
    emptySlotsLabel.setText("Empty - chain is clean", juce::dontSendNotification);
    slotContent.addAndMakeVisible(emptySlotsLabel);

    addVstButton.setColour(juce::TextButton::buttonColourId, theme::control());
    addVstButton.setColour(juce::TextButton::textColourOffId, theme::violet());
    addVstButton.onClick = [this] { openPicker(); };
    slotContent.addAndMakeVisible(addVstButton);

    slotViewport.setViewedComponent(&slotContent, false);
    slotViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(slotViewport);

    noEditorLabel.setJustificationType(juce::Justification::centred);
    noEditorLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
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
    outputHeader.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    outputHeader.setColour(juce::Label::textColourId, theme::amber());
    addAndMakeVisible(outputHeader);

    emptyLabel.setText("No chains yet - add a channel or output in the Mixer view.",
                       juce::dontSendNotification);
    emptyLabel.setJustificationType(juce::Justification::centred);
    emptyLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    emptyLabel.setColour(juce::Label::textColourId, theme::dim());
    emptyLabel.setVisible(false);
    addAndMakeVisible(emptyLabel);

    rebuild();
}

RackView::~RackView() {
    closeEqWindow();
    closeDockedEditor();
}

std::uint32_t RackView::selectedChainId() const {
    return selectedId < 0 ? 0 : static_cast<std::uint32_t>(selectedId);
}

void RackView::setSelectedChannel(int channelId, bool isOutput) {
    if (channelId == selectedId && isOutput == selectedIsOutput && channelId >= 0) return;
    closeEqWindow();
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
        meter = std::make_unique<LevelMeter>(ch->peak);
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
        meter = std::make_unique<LevelMeter>(out->peak);
    }
    if (meter != nullptr) addAndMakeVisible(*meter);
    refreshEqButton();
}

void RackView::refreshSlots() {
    slots.clear();
    const auto* inserts = currentInserts();
    const int count = inserts == nullptr ? 0 : static_cast<int>(inserts->size());
    for (int i = 0; i < count; ++i) {
        auto* slot = slots.add(new InsertSlotComponent(*this, i));
        slot->setInsert((*inserts)[static_cast<std::size_t>(i)].get());
        slotContent.addAndMakeVisible(*slot);
    }
    emptySlotsLabel.setVisible(slots.isEmpty());

    const int innerW = juce::jmax(0, slotViewport.getWidth() - 8);
    int y = 4;
    for (auto* slot : slots) {
        slot->setBounds(4, y, innerW, kSlotRowHeight);
        y += kSlotRowHeight + kSlotGap;
    }
    if (slots.isEmpty()) {
        emptySlotsLabel.setBounds(4, y, juce::jmax(120, innerW), kSlotRowHeight);
        y += kSlotRowHeight + kSlotGap;
    }
    addVstButton.setBounds(4, y, kAddButtonWidth, kSlotRowHeight);
    y += kSlotRowHeight + kSlotGap;
    slotContent.setSize(juce::jmax(0, slotViewport.getWidth()), juce::jmax(0, y));
}

void RackView::refreshEditor() {
    if (eqFocused && eqWindow != nullptr) {
        closeDockedEditor();
        if (eqWindow->getParentComponent() != &editorHolder)
            editorHolder.addAndMakeVisible(eqWindow);
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
        resized();
        return;
    }
    if (eqFocused) {
        eqFocused = false;
    }
    closeDockedEditor();
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
        noEditorLabel.setText("No plugins - click + Add VST3 above", juce::dontSendNotification);
        noEditorLabel.setVisible(true);
        noEditorBypass.setVisible(false);
        dockedEditor = nullptr;
        dockedHost = nullptr;
        resized();
        return;
    }

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
    if (eqWindow != nullptr) {
        eqFocused = true;
        closeDockedEditor();
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
            liveRateHz, true);
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
        eqWindow = window;
        eqFocused = true;
        closeDockedEditor();
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
            liveRateHz, true);
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
        eqWindow = window;
        eqFocused = true;
        closeDockedEditor();
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
}

void RackView::refreshEqButton() {
    bool active = false;
    if (auto* ch = currentChannel()) {
        if (!ch->eqBypassed.get()) {
            for (const auto& band : ch->eqBands) {
                if (band.enabled) { active = true; break; }
            }
        }
    } else if (auto* out = currentOutput()) {
        if (!out->eqBypassed.get()) {
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
    eqButton.setVisible(hasChain);
    slotViewport.setVisible(hasChain);
    editorViewport.setVisible(hasChain);
    outputHeader.setVisible(hasChain);
    for (auto* b : outputButtons) b->setVisible(hasChain);

    if (!hasChain) {
        closeEqWindow();
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
    la.removeFromTop(6);

    trimCaption.setBounds(la.removeFromTop(14));
    auto trimArea = la.removeFromTop(56);
    trimKnob.setBounds(trimArea.withSizeKeepingCentre(52, 52));
    la.removeFromTop(6);

    auto btnRow = la.removeFromBottom(24);
    la.removeFromBottom(8);

    auto panArea = la.removeFromBottom(92);
    panCaption.setBounds(panArea.removeFromTop(14));
    panSlider.setBounds(panArea.withSizeKeepingCentre(64, 64));
    la.removeFromBottom(8);

    auto faderRow = la;
    auto meterArea = faderRow.removeFromRight(14);
    faderRow.removeFromRight(4);
    gainCaption.setBounds(faderRow.removeFromTop(14));
    auto faderArea = faderRow.reduced(0, 4);
    if (meter != nullptr) meter->setBounds(meterArea.reduced(0, 4));
    gainSlider.setBounds(faderArea);

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
    pa.removeFromTop(14);
    insertsHeader.setBounds(pa.removeFromTop(20));
    pa.removeFromTop(4);
    eqButton.setBounds(pa.removeFromTop(30));
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
        const int ew = juce::jmax(400, EQWindow::kWidth);
        const int eh = juce::jmax(320, EQWindow::kHeight);
        editorHolder.setSize(juce::jmax(vw, ew), juce::jmax(vh, eh));
        eqWindow->setBounds(editorHolder.getLocalBounds().withSizeKeepingCentre(ew, eh));
        noEditorLabel.setVisible(false);
        noEditorBypass.setVisible(false);
    }
    if (dockedEditor != nullptr) {
        const int ew = juce::jmax(200, dockedEditor->getWidth());
        const int eh = juce::jmax(150, dockedEditor->getHeight());
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
        g.setColour(fill);
        g.fillRoundedRectangle(bounds.toFloat(), 3.0f);
        g.setColour(outline);
        g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 3.0f, 1.0f);
    };

    drawPanel(leftArea, theme::panel().darker(0.08f), theme::border());
    drawPanel(pluginArea, theme::panel(), theme::border());
    drawPanel(centerArea, theme::bg().darker(0.16f), theme::line().withAlpha(0.8f));
    drawPanel(rightArea, theme::panel().darker(0.08f), theme::border());

    g.setColour(theme::dim().withAlpha(0.75f));
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    g.drawText("INPUT", leftArea.getX() + 8, leftArea.getY() + 4, 60, 12,
               juce::Justification::left, false);
    g.drawText("INSERTS", pluginArea.getX() + 8, pluginArea.getY() + 4, 70, 12,
               juce::Justification::left, false);
    g.drawText("PLUGIN EDITOR", centerArea.getX() + 10, centerArea.getY() + 6, 100, 12,
               juce::Justification::left, false);
    g.drawText("OUTPUT", rightArea.getX() + 8, rightArea.getY() + 4, 60, 12,
               juce::Justification::left, false);
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
    focusedInsertId = insert->id;
    refreshEditor();
    return true;
}