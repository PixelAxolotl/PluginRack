#include "EQWindow.h"

EQWindow::EQWindow(const juce::String& title,
                   const std::vector<pluginrack::EqBand>& initialBands,
                   std::function<void(const std::vector<pluginrack::EqBand>&)> applyBandsCallback)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      applyBands(std::move(applyBandsCallback)) {
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    setResizeLimits(400, 320, 1000, 700);

    curve.setBands(initialBands);
    curve.onBandsChanged = [this] {
        if (applyBands) applyBands(curve.getBands());
        syncControls(); // node drag moved freq/gain: keep knobs in step
    };
    curve.onBandSelected = [this] { syncControls(); };
    // Intentional direct children (no content component): the sanctioned
    // base-class call documented by ResizableWindow's shadowing overload.
    Component::addAndMakeVisible(curve);

    autoButton.setTooltip("Automatic band assist - visual only in v1");
    autoButton.setEnabledState(false); // plan: no function in v1
    Component::addAndMakeVisible(autoButton);

    stereoTag.setJustificationType(juce::Justification::centred);
    stereoTag.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    stereoTag.setColour(juce::Label::textColourId, theme::dim());
    stereoTag.setTooltip("Stereo processing - visual only in v1");
    Component::addAndMakeVisible(stereoTag);

    bypassButton.setTooltip("Bypass the selected band");
    bypassButton.setClickAction([this] {
        auto bands = curve.getBands();
        const int sel = curve.getSelectedBand();
        if (sel < 0 || sel >= static_cast<int>(bands.size())) return;
        bands[static_cast<std::size_t>(sel)].enabled = !bands[static_cast<std::size_t>(sel)].enabled;
        curve.setBands(bands);
        bypassButton.setToggleState(!bands[static_cast<std::size_t>(sel)].enabled);
        if (applyBands) applyBands(curve.getBands());
    });
    Component::addAndMakeVisible(bypassButton);

    soloButton.setTooltip("Solo the selected band (audible bands: solo + enabled)");
    soloButton.setClickAction([this] {
        auto bands = curve.getBands();
        const int sel = curve.getSelectedBand();
        if (sel < 0 || sel >= static_cast<int>(bands.size())) return;
        bands[static_cast<std::size_t>(sel)].solo = !bands[static_cast<std::size_t>(sel)].solo;
        curve.setBands(bands);
        soloButton.setToggleState(bands[static_cast<std::size_t>(sel)].solo);
        if (applyBands) applyBands(curve.getBands());
    });
    Component::addAndMakeVisible(soloButton);

    for (auto* label : { &freqLabel, &gainLabel, &qLabel }) {
        label->setJustificationType(juce::Justification::centred);
        label->setFont(juce::Font(juce::FontOptions(11.0f)));
        label->setColour(juce::Label::textColourId, theme::dim());
        Component::addAndMakeVisible(*label);
    }

    // Freq/Gain/Q knobs for the selected band, styled like the strip knobs
    // (ConsoleLookAndFeel conventions: control box, themed arcs).
    freqKnob.setSliderStyle(juce::Slider::RotaryHorizontalDrag);
    freqKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 16);
    freqKnob.setRange(EQCurveComponent::kMinFreqHz, EQCurveComponent::kMaxFreqHz);
    freqKnob.setSkewFactorFromMidPoint(1000.0);
    freqKnob.textFromValueFunction = [](double hz) {
        return hz >= 1000.0 ? juce::String(hz / 1000.0, 2) + " kHz"
                            : juce::String(hz, 0) + " Hz";
    };
    freqKnob.valueFromTextFunction = [](const juce::String& text) {
        const double hz = text.getDoubleValue();
        return juce::jlimit(static_cast<double>(EQCurveComponent::kMinFreqHz),
                            static_cast<double>(EQCurveComponent::kMaxFreqHz), hz);
    };
    freqKnob.onValueChange = [this] { applyFromControls(); };
    freqKnob.setColour(juce::Slider::rotarySliderFillColourId, theme::cyan());
    freqKnob.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
    freqKnob.setColour(juce::Slider::textBoxTextColourId, theme::text());
    freqKnob.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    Component::addAndMakeVisible(freqKnob);

    gainKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    gainKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 16);
    gainKnob.setRange(-EQCurveComponent::kMaxGainDb, EQCurveComponent::kMaxGainDb, 0.1);
    gainKnob.setTextValueSuffix(" dB");
    gainKnob.setDoubleClickReturnValue(true, 0.0);
    gainKnob.onValueChange = [this] { applyFromControls(); };
    gainKnob.setColour(juce::Slider::rotarySliderFillColourId, theme::green());
    gainKnob.setColour(juce::Slider::rotarySliderOutlineColourId, theme::deepGreen());
    gainKnob.setColour(juce::Slider::textBoxTextColourId, theme::text());
    gainKnob.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    Component::addAndMakeVisible(gainKnob);

    qKnob.setSliderStyle(juce::Slider::RotaryHorizontalDrag);
    qKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 16);
    qKnob.setRange(EQCurveComponent::kMinQ, EQCurveComponent::kMaxQ);
    qKnob.setSkewFactorFromMidPoint(1.0);
    qKnob.textFromValueFunction = [](double q) { return juce::String(q, 2); };
    qKnob.valueFromTextFunction = [](const juce::String& text) {
        const double q = text.getDoubleValue();
        return juce::jlimit(static_cast<double>(EQCurveComponent::kMinQ),
                            static_cast<double>(EQCurveComponent::kMaxQ), q);
    };
    qKnob.setDoubleClickReturnValue(true, 0.707);
    qKnob.onValueChange = [this] { applyFromControls(); };
    qKnob.setColour(juce::Slider::rotarySliderFillColourId, theme::violet());
    qKnob.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
    qKnob.setColour(juce::Slider::textBoxTextColourId, theme::text());
    qKnob.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    Component::addAndMakeVisible(qKnob);

    syncControls();

    centreWithSize(kWidth, kHeight);
    setVisible(true);
}

void EQWindow::applyFromControls() {
    auto bands = curve.getBands();
    const int sel = curve.getSelectedBand();
    if (sel < 0 || sel >= static_cast<int>(bands.size())) return;
    pluginrack::EqBand& band = bands[static_cast<std::size_t>(sel)];

    // Sliders self-clamp to their ranges; curve bounds equal the validator
    // bounds, so no extra guarding needed here.
    band.freqHz = static_cast<float>(freqKnob.getValue());
    band.gainDb = static_cast<float>(gainKnob.getValue());
    band.q = static_cast<float>(qKnob.getValue());

    curve.setBands(bands); // repaint with the new value
    if (applyBands) applyBands(curve.getBands());
}

void EQWindow::syncControls() {
    const auto& bands = curve.getBands();
    const int sel = curve.getSelectedBand();
    if (sel < 0 || sel >= static_cast<int>(bands.size())) return;
    const pluginrack::EqBand& band = bands[static_cast<std::size_t>(sel)];

    freqKnob.setValue(band.freqHz, juce::dontSendNotification);
    gainKnob.setValue(band.gainDb, juce::dontSendNotification);
    qKnob.setValue(band.q, juce::dontSendNotification);
    bypassButton.setToggleState(!band.enabled);
    soloButton.setToggleState(band.solo);

    // High-pass ignores gain and Q in the engine; disable knobs that would
    // do nothing so the UI never lies about its effect.
    const bool isHighPass = (band.type == pluginrack::EqType::HighPass);
    gainKnob.setEnabled(!isHighPass);
    qKnob.setEnabled(!isHighPass);
}

void EQWindow::closeButtonPressed() {
    // Deleting a window from inside its own close-button callback is fragile:
    // defer so the button can finish dispatching first. The owner may destroy
    // the window before the message arrives, so guard with a weak pointer.
    juce::Component::SafePointer<EQWindow> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        if (safe->onCloseRequest) safe->onCloseRequest();
    });
}

void EQWindow::resized() {
    // With a native title bar the component bounds *are* the content area.
    auto area = getLocalBounds().reduced(4);
    auto controlsRow = area.removeFromBottom(104);
    curve.setBounds(area);

    auto left = controlsRow.removeFromLeft(64);
    left.removeFromTop(6);
    autoButton.setBounds(left.removeFromTop(22));
    left.removeFromTop(4);
    stereoTag.setBounds(left.removeFromTop(16));
    controlsRow.removeFromLeft(8);

    auto right = controlsRow.removeFromRight(64);
    right.removeFromTop(6);
    bypassButton.setBounds(right.removeFromTop(22));
    right.removeFromTop(4);
    soloButton.setBounds(right.removeFromTop(22));

    // Three label+knob slots across the remaining middle.
    const int knobW = juce::jmax(1, controlsRow.getWidth() / 3);
    auto slot = [&](int i) {
        return juce::Rectangle<int>(controlsRow.getX() + i * knobW, controlsRow.getY(),
                                    knobW, controlsRow.getHeight());
    };
    for (int i = 0; i < 3; ++i) {
        auto s = slot(i);
        auto* label = i == 0 ? &freqLabel : (i == 1 ? &gainLabel : &qLabel);
        auto* knob = i == 0 ? &freqKnob : (i == 1 ? &gainKnob : &qKnob);
        label->setBounds(s.removeFromTop(14));
        knob->setBounds(s.removeFromTop(66));
    }
}
