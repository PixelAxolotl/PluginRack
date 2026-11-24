#include "SideChainBar.h"

#include <algorithm>

#include "Theme.h"

namespace {
constexpr int kSourceNone = 1;
constexpr int kSourceChannel = 2;
constexpr int kSourceOutput = 3;
constexpr int kSourceBus = 4;

constexpr int kTapPreInsert = 1;
constexpr int kTapPostFader = 2;

int sourceToItem(pluginrack::SideChainSource source) {
    switch (source) {
        case pluginrack::SideChainSource::Channel: return kSourceChannel;
        case pluginrack::SideChainSource::Output: return kSourceOutput;
        case pluginrack::SideChainSource::Bus: return kSourceBus;
        case pluginrack::SideChainSource::None: break;
    }
    return kSourceNone;
}

pluginrack::SideChainSource itemToSource(int item) {
    switch (item) {
        case kSourceChannel: return pluginrack::SideChainSource::Channel;
        case kSourceOutput: return pluginrack::SideChainSource::Output;
        case kSourceBus: return pluginrack::SideChainSource::Bus;
        default: break;
    }
    return pluginrack::SideChainSource::None;
}

int tapToItem(pluginrack::SideChainTap tap) {
    return tap == pluginrack::SideChainTap::PreInsert ? kTapPreInsert : kTapPostFader;
}

pluginrack::SideChainTap itemToTap(int item) {
    return item == kTapPreInsert ? pluginrack::SideChainTap::PreInsert
                                 : pluginrack::SideChainTap::PostFader;
}

juce::String channelName(const AudioEngine::Channel& channel) {
    return channel.name.isNotEmpty() ? channel.name : "Channel " + juce::String(channel.id);
}

juce::String outputName(const AudioEngine::Output& output) {
    return output.name.isNotEmpty() ? output.name : "Output " + juce::String(output.id);
}
} // namespace

SideChainBar::SideChainBar(AudioEngine& engine) : audioEngine(engine) {
    caption.setText("Side chain", juce::dontSendNotification);
    caption.setJustificationType(juce::Justification::centredLeft);
    caption.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    caption.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(caption);

    const auto styleCombo = [](juce::ComboBox& combo) {
        combo.setColour(juce::ComboBox::backgroundColourId, theme::control());
        combo.setColour(juce::ComboBox::textColourId, theme::text());
        combo.setColour(juce::ComboBox::arrowColourId, theme::dim());
        combo.setColour(juce::ComboBox::outlineColourId, theme::border());
    };

    sourceCombo.addItem("None", kSourceNone);
    sourceCombo.addItem("Channel", kSourceChannel);
    sourceCombo.addItem("Output", kSourceOutput);
    sourceCombo.addItem("Bus", kSourceBus);
    sourceCombo.setSelectedId(kSourceNone, juce::dontSendNotification);
    sourceCombo.setTooltip("Where this insert's side-chain signal comes from");
    sourceCombo.onChange = [this] {
        if (updating) return;
        rebuildSourceIds();
        apply();
    };
    styleCombo(sourceCombo);
    addAndMakeVisible(sourceCombo);

    sourceIdCombo.setTooltip("The channel, output or aux bus feeding the side-chain");
    sourceIdCombo.onChange = [this] {
        if (!updating) apply();
    };
    styleCombo(sourceIdCombo);
    addAndMakeVisible(sourceIdCombo);

    tapCombo.addItem("Pre-insert", kTapPreInsert);
    tapCombo.addItem("Post-fader", kTapPostFader);
    tapCombo.setSelectedId(kTapPostFader, juce::dontSendNotification);
    tapCombo.setTooltip("Tap the source before its inserts or after its fader");
    tapCombo.onChange = [this] {
        if (!updating) apply();
    };
    styleCombo(tapCombo);
    addAndMakeVisible(tapCombo);

    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
    gainSlider.setRange(-60.0, 12.0, 0.1);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setDoubleClickReturnValue(true, 0.0);
    gainSlider.setColour(juce::Slider::trackColourId, theme::deepGreen());
    gainSlider.setColour(juce::Slider::backgroundColourId, theme::strip());
    gainSlider.setColour(juce::Slider::thumbColourId, theme::green());
    gainSlider.setColour(juce::Slider::textBoxTextColourId, theme::text());
    gainSlider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    gainSlider.setTooltip("Side-chain input trim");
    gainSlider.onValueChange = [this] {
        if (!updating) apply();
    };
    addAndMakeVisible(gainSlider);

    addBusButton.setColour(juce::TextButton::buttonColourId, theme::control());
    addBusButton.setColour(juce::TextButton::textColourOffId, theme::cyan());
    addBusButton.setTooltip("Create a dedicated aux bus for side-chain use");
    addBusButton.onClick = [this] { addBus(); };
    addAndMakeVisible(addBusButton);

    sourcesButton.setColour(juce::TextButton::buttonColourId, theme::control());
    sourcesButton.setColour(juce::TextButton::textColourOffId, theme::text());
    sourcesButton.setTooltip("Choose which channels feed this aux bus");
    sourcesButton.onClick = [this] { editBusSources(); };
    addAndMakeVisible(sourcesButton);
    sourcesButton.setVisible(false);

    keyLevel.setText("Key -inf", juce::dontSendNotification);
    keyLevel.setJustificationType(juce::Justification::centredRight);
    keyLevel.setTooltip("Peak level of the side-chain key reaching this insert");
    keyLevel.setColour(juce::Label::textColourId, theme::dim());
    keyLevel.setFont(juce::Font(juce::FontOptions(11.0f)));
    addAndMakeVisible(keyLevel);
    startTimerHz(20);
}

void SideChainBar::timerCallback() {
    if (target == nullptr || selectedSource() == pluginrack::SideChainSource::None) {
        keyLevel.setText("Key -inf", juce::dontSendNotification);
        return;
    }
    const float peak = audioEngine.getSideChainKeyPeak(ownerId, target->id,
                                                       ownerKind != OwnerKind::Channel);
    if (peak <= 0.0f) {
        keyLevel.setText("Key -inf", juce::dontSendNotification);
        return;
    }
    const float db = juce::Decibels::gainToDecibels(peak, -100.0f);
    keyLevel.setText("Key " + juce::String(db, 1) + " dB", juce::dontSendNotification);
}

void SideChainBar::setTarget(AudioEngine::InsertSlot* insert, OwnerKind kind,
                             std::uint32_t ownerIdIn) {
    if (target == insert && ownerKind == kind && ownerId == ownerIdIn) return;
    target = insert;
    ownerKind = kind;
    ownerId = ownerIdIn;
    refreshValues();
}

void SideChainBar::refresh() {
    refreshValues();
}

pluginrack::SideChainSource SideChainBar::selectedSource() const {
    return itemToSource(sourceCombo.getSelectedId());
}

void SideChainBar::rebuildSourceIds() {
    const auto source = selectedSource();
    sourceIdCombo.clear(juce::dontSendNotification);
    if (source == pluginrack::SideChainSource::Channel) {
        for (const auto& channel : audioEngine.getChannels()) {
            if (channel == nullptr || channel->id == 0) continue;
            sourceIdCombo.addItem(channelName(*channel), static_cast<int>(channel->id));
        }
    } else if (source == pluginrack::SideChainSource::Output) {
        for (const auto& output : audioEngine.getOutputs()) {
            if (output == nullptr || output->id == 0) continue;
            sourceIdCombo.addItem(outputName(*output), static_cast<int>(output->id));
        }
    } else if (source == pluginrack::SideChainSource::Bus) {
        for (const auto& bus : audioEngine.getBuses()) {
            if (bus == nullptr || bus->id == 0) continue;
            sourceIdCombo.addItem(bus->name.isNotEmpty() ? bus->name
                                                         : "Bus " + juce::String(bus->id),
                                  static_cast<int>(bus->id));
        }
    }
    sourceIdCombo.setEnabled(source != pluginrack::SideChainSource::None);
    if (sourceIdCombo.getNumItems() > 0 && sourceIdCombo.getSelectedId() == 0) {
        sourceIdCombo.setSelectedItemIndex(0, juce::dontSendNotification);
    }
    sourcesButton.setVisible(source == pluginrack::SideChainSource::Bus);
    // A bus key is summed post-gain, so the tap and the input trim do not
    // apply to it.
    const bool busSource = source == pluginrack::SideChainSource::Bus;
    tapCombo.setVisible(!busSource);
    gainSlider.setVisible(!busSource);
    if (busSource) {
        tapCombo.setBounds({});
        gainSlider.setBounds({});
    }
    // setVisible() does not run the parent's layout, and resized() only gives
    // the Sources button bounds while it is visible. Re-lay the bar here or a
    // newly shown button keeps its stale zero-size bounds until the next
    // window resize.
    resized();
}

void SideChainBar::refreshValues() {
    if (target == nullptr) return;
    updating = true;
    sourceCombo.setSelectedId(sourceToItem(target->sideChainSource),
                              juce::dontSendNotification);
    rebuildSourceIds();
    if (target->sideChainSource != pluginrack::SideChainSource::None
        && target->sideChainSourceId != 0) {
        sourceIdCombo.setSelectedId(static_cast<int>(target->sideChainSourceId),
                                    juce::dontSendNotification);
    }
    tapCombo.setSelectedId(tapToItem(target->sideChainTap), juce::dontSendNotification);
    gainSlider.setValue(target->sideChainGainDb.get(), juce::dontSendNotification);
    updating = false;
}

void SideChainBar::apply() {
    if (target == nullptr || updating) return;
    const auto source = selectedSource();
    const std::uint32_t sourceId =
        source == pluginrack::SideChainSource::None
            ? 0u
            : static_cast<std::uint32_t>(juce::jmax(0, sourceIdCombo.getSelectedId()));
    const auto tap = itemToTap(tapCombo.getSelectedId());
    const auto gainDb = static_cast<float>(gainSlider.getValue());
    if (ownerKind == OwnerKind::Channel) {
        audioEngine.setPluginSideChain(ownerId, target->id, source, sourceId, tap, gainDb);
    } else {
        // Outputs and buses share the strip-side insert API.
        audioEngine.setOutputPluginSideChain(ownerId, target->id, source, sourceId, tap, gainDb);
    }
}

void SideChainBar::addBus() {
    std::vector<std::uint32_t> sources;
    if (ownerKind == OwnerKind::Channel) {
        // The strip the insert lives on is the natural first feed; a snare
        // insert usually side-chains from its own channel.
        sources.push_back(ownerId);
    } else if (ownerKind == OwnerKind::Output) {
        for (const auto& channel : audioEngine.getChannels()) {
            if (channel != nullptr && channel->id != 0) sources.push_back(channel->id);
        }
    }
    const auto busId = audioEngine.addBus({});
    if (busId == 0) return;
    for (const auto channelId : sources) {
        audioEngine.setBusSend(channelId, busId, 0.0f, 0.0f);
    }
    updating = true;
    sourceCombo.setSelectedId(kSourceBus, juce::dontSendNotification);
    rebuildSourceIds();
    sourceIdCombo.setSelectedId(static_cast<int>(busId), juce::dontSendNotification);
    updating = false;
    apply();
}

void SideChainBar::editBusSources() {
    if (sourceIdCombo.getSelectedId() <= 0) return;
    const auto busId = static_cast<std::uint32_t>(sourceIdCombo.getSelectedId());
    const auto* bus = audioEngine.getBusById(busId);
    if (bus == nullptr) return;

    const auto hasSource = [this, busId](std::uint32_t channelId) {
        for (const auto& send : audioEngine.getBusSends()) {
            if (send != nullptr && send->channelId == channelId && send->busId == busId
                && (send->sendGainDbL.get() > AudioEngine::kSendOffDb
                    || send->sendGainDbR.get() > AudioEngine::kSendOffDb)) {
                return true;
            }
        }
        return false;
    };

    juce::PopupMenu menu;
    menu.addSectionHeader(bus->name.isNotEmpty() ? bus->name
                                                 : "Bus " + juce::String(bus->id));
    for (const auto& channel : audioEngine.getChannels()) {
        if (channel == nullptr || channel->id == 0) continue;
        menu.addItem(static_cast<int>(channel->id), channelName(*channel), true,
                     hasSource(channel->id));
    }
    menu.addSeparator();
    menu.addItem(9000, "Remove this bus");

    juce::Component::SafePointer<SideChainBar> safe(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&sourcesButton),
        [safe, busId](int result) {
            if (safe == nullptr || result == 0) return;
            if (safe->audioEngine.getBusById(busId) == nullptr) return;
            if (result == 9000) {
                safe->audioEngine.removeBus(busId);
                safe->sourceCombo.setSelectedId(kSourceNone, juce::dontSendNotification);
                safe->refreshValues();
                return;
            }
            const auto channelId = static_cast<std::uint32_t>(result);
            for (const auto& send : safe->audioEngine.getBusSends()) {
                if (send != nullptr && send->channelId == channelId && send->busId == busId) {
                    const bool on = send->sendGainDbL.get() > AudioEngine::kSendOffDb
                                    || send->sendGainDbR.get() > AudioEngine::kSendOffDb;
                    safe->audioEngine.setBusSend(
                        channelId, busId,
                        on ? AudioEngine::kSendOffDb : 0.0f,
                        on ? AudioEngine::kSendOffDb : 0.0f);
                    return;
                }
            }
            safe->audioEngine.setBusSend(channelId, busId, 0.0f, 0.0f);
        });
}

void SideChainBar::paint(juce::Graphics& g) {
    g.fillAll(theme::strip());
    g.setColour(theme::border());
    g.fillRect(0, getHeight() - 1, getWidth(), 1);
}

void SideChainBar::resized() {
    auto area = getLocalBounds().reduced(6, 6);
    const int gap = 4;

    caption.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(gap);
    sourceCombo.setBounds(area.removeFromLeft(88));
    area.removeFromLeft(gap);
    if (tapCombo.isVisible()) {
        tapCombo.setBounds(area.removeFromLeft(96));
        area.removeFromLeft(gap);
    }
    if (gainSlider.isVisible()) {
        gainSlider.setBounds(area.removeFromLeft(150));
        area.removeFromLeft(gap);
    }
    keyLevel.setBounds(area.removeFromRight(96));
    area.removeFromRight(gap);

    // Buttons keep their size; the source list takes what is left.
    if (sourcesButton.isVisible()) {
        sourcesButton.setBounds(area.removeFromRight(72));
        area.removeFromRight(gap);
    }
    addBusButton.setBounds(area.removeFromRight(56));
    area.removeFromRight(gap);
    sourceIdCombo.setBounds(area);
}
