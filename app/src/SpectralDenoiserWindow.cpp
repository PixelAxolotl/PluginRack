#include "SpectralDenoiserWindow.h"

#include "AudioEngine.h"

#include <algorithm>

SpectralDenoiserWindow::SpectralDenoiserWindow(const juce::String& title,
                                               const pluginrack::SpectralDenoiserEffect& initialEffect,
                                               AudioEngine& engine, bool isOutput,
                                               std::uint32_t stripId, ApplyCallback applyCallback)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      effect(initialEffect), apply(std::move(applyCallback)), audioEngine(engine),
      outputStrip(isOutput), targetId(stripId) {
    setResizable(false, false);
    setContentOwned(&content, false);
    setUsingNativeTitleBar(true);
    centreWithSize(kWidth, kHeight);

    theme::ui::styleTitleLabel(modeLabel);
    content.addAndMakeVisible(modeLabel);
    content.addAndMakeVisible(enableButton);
    enableButton.setClickingTogglesState(true);
    enableButton.setToggleState(!effect.bypassed, juce::dontSendNotification);
    theme::ui::styleButton(enableButton);
    enableButton.onClick = [this] {
        effect.bypassed = !enableButton.getToggleState();
        if (apply) apply(effect, true);
    };

    theme::ui::styleCombo(modeCombo);
    modeCombo.addItem("Adaptive", 1);
    modeCombo.addItem("Learn", 2);
    modeCombo.setSelectedId(effect.mode == pluginrack::SpectralDenoiserMode::Learn ? 2 : 1,
                            juce::dontSendNotification);
    content.addAndMakeVisible(modeCombo);
    modeCombo.onChange = [this] {
        effect.mode = modeCombo.getSelectedId() == 2
            ? pluginrack::SpectralDenoiserMode::Learn
            : pluginrack::SpectralDenoiserMode::Adaptive;
        if (apply) apply(effect, false);
        refreshStatus();
    };

    theme::ui::styleButton(learnButton);
    content.addAndMakeVisible(learnButton);
    learnButton.onClick = [this] { startLearning(); };

    theme::ui::styleButton(clearButton);
    content.addAndMakeVisible(clearButton);
    clearButton.onClick = [this] { clearProfile(); };

    content.addAndMakeVisible(reduction);
    content.addAndMakeVisible(reductionLabel);
    reduction.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    reduction.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 18);
    reduction.setRange(pluginrack::kSpectralDenoiserMinReductionDb,
                       pluginrack::kSpectralDenoiserMaxReductionDb, 0.5);
    reduction.setTextValueSuffix(" dB");
    reduction.setDoubleClickReturnValue(true, pluginrack::kSpectralDenoiserDefaultReductionDb);
    reduction.setColour(juce::Slider::rotarySliderFillColourId, theme::cyan());
    reduction.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
    reduction.setColour(juce::Slider::textBoxTextColourId, theme::text());
    reduction.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    reduction.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    reduction.setValue(effect.reductionDb, juce::dontSendNotification);
    reduction.onValueChange = [this] {
        effect.reductionDb = static_cast<float>(reduction.getValue());
        if (apply) apply(effect, false);
    };

    reductionLabel.setJustificationType(juce::Justification::centred);
    reductionLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    reductionLabel.setColour(juce::Label::textColourId, theme::dim());

    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    statusLabel.setColour(juce::Label::textColourId, theme::dim());
    content.addAndMakeVisible(statusLabel);

    refreshStatus();
    setVisible(true);
    startTimer(100);
}

void SpectralDenoiserWindow::startLearning() {
    if (apply) apply(effect, false);
    if (outputStrip) audioEngine.startOutputDenoiserLearn(targetId, effect.id);
    else audioEngine.startChannelDenoiserLearn(targetId, effect.id);
    learning = true;
    statusLabel.setText("Learning noise profile... 0%", juce::dontSendNotification);
}

void SpectralDenoiserWindow::clearProfile() {
    effect.noiseProfile.clear();
    effect.profileSampleRate = 0.0;
    learning = false;
    if (apply) apply(effect, false);
    refreshStatus();
}

void SpectralDenoiserWindow::refreshStatus() {
    if (learning) return;
    if (effect.hasProfile()) {
        statusLabel.setText("Noise profile saved", juce::dontSendNotification);
    } else if (effect.mode == pluginrack::SpectralDenoiserMode::Learn) {
        statusLabel.setText("Learn a noise-only profile to reduce noise",
                            juce::dontSendNotification);
    } else {
        statusLabel.setText("Adaptive: tracks the noise floor as it plays",
                            juce::dontSendNotification);
    }
}

void SpectralDenoiserWindow::timerCallback() {
    if (!learning) return;

    const float progress = outputStrip
        ? audioEngine.getOutputDenoiserLearnProgress(targetId, effect.id)
        : audioEngine.getChannelDenoiserLearnProgress(targetId, effect.id);

    if (progress < 1.0f) {
        const int percent = juce::jlimit(0, 99, static_cast<int>(progress * 100.0f));
        statusLabel.setText("Learning noise profile... " + juce::String(percent) + "%",
                            juce::dontSendNotification);
        return;
    }

    std::vector<float> profile;
    double profileRate = 0.0;
    const bool took = outputStrip
        ? audioEngine.takeOutputDenoiserLearnedProfile(targetId, effect.id, profile, profileRate)
        : audioEngine.takeChannelDenoiserLearnedProfile(targetId, effect.id, profile, profileRate);

    learning = false;
    if (took) {
        effect.noiseProfile = std::move(profile);
        effect.profileSampleRate = profileRate;
        effect.mode = pluginrack::SpectralDenoiserMode::Learn;
        modeCombo.setSelectedId(2, juce::dontSendNotification);
        if (apply) apply(effect, false);
        statusLabel.setText("Noise profile saved", juce::dontSendNotification);
    } else {
        refreshStatus();
    }
}

void SpectralDenoiserWindow::setBypassed(bool b) {
    effect.bypassed = b;
    enableButton.setToggleState(!b, juce::dontSendNotification);
}

void SpectralDenoiserWindow::resized() {
    auto* installedContent = getContentComponent();
    if (installedContent == nullptr) return;
    installedContent->setBounds(getLocalBounds());

    auto area = installedContent->getLocalBounds().reduced(theme::ui::kEffectPad);
    const auto header = theme::ui::layoutEffectHeader(area.removeFromTop(theme::ui::kEffectHeaderH));
    modeLabel.setBounds(header.title.reduced(12, 0));
    enableButton.setBounds(header.enable);

    area.removeFromTop(10);
    auto topRow = area.removeFromTop(28);
    modeCombo.setBounds(topRow.removeFromLeft(120));
    topRow.removeFromLeft(8);
    learnButton.setBounds(topRow.removeFromLeft(84));
    topRow.removeFromLeft(8);
    clearButton.setBounds(topRow.removeFromLeft(110));
    topRow.removeFromLeft(12);
    statusLabel.setBounds(topRow);

    area.removeFromTop(12);
    const auto knobArea = area.removeFromTop(juce::jmin(180, area.getHeight()));
    theme::ui::placeWindowKnob(knobArea.withSizeKeepingCentre(150, knobArea.getHeight()),
                               reductionLabel, reduction);
}
