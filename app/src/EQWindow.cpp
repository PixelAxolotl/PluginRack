#include "EQWindow.h"

#include <cmath>

EQWindow::EQWindow(const juce::String& title,
                   const std::vector<pluginrack::EqBand>& initialBands,
                   std::function<void(const std::vector<pluginrack::EqBand>&)> applyBandsCallback,
                   double sampleRateHz)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      applyBands(std::move(applyBandsCallback)) {
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    setResizeLimits(400, 320, 1000, 700);

    curve.setSampleRateHz(sampleRateHz); // live rate before the first paint
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

    // --- analyzer (Task 6): one FFT per open window, everything allocated
    // here on the message thread so the timer callback never touches the
    // heap. A Hann window keeps spectral leakage down; the tap rings arrive
    // through pullAnalyzerTap, wired by the owning strip.
    analyzerFft = std::make_unique<juce::dsp::FFT>(11); // 2^11 = 2048
    hannCoeffs.resize(static_cast<std::size_t>(kAnalyzerFftSize));
    for (int i = 0; i < kAnalyzerFftSize; ++i) {
        const double t = juce::MathConstants<double>::twoPi * i / kAnalyzerFftSize;
        hannCoeffs[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(t));
    }
    fftScratch.assign(static_cast<std::size_t>(kAnalyzerFftSize) * 2, 0.0f);
    tapL.assign(static_cast<std::size_t>(kAnalyzerFftSize), 0.0f);
    tapR.assign(static_cast<std::size_t>(kAnalyzerFftSize), 0.0f);
    spectrumDb.assign(static_cast<std::size_t>(kAnalyzerFftSize) / 2 + 1, 0.0f);
    analyzerPreButton.setTooltip("Analyzer: spectrum before the EQ tap");
    analyzerPreButton.setClickAction([this] { setAnalyzerMode(kAnalyzerPre); });
    Component::addAndMakeVisible(analyzerPreButton);
    analyzerPostButton.setTooltip("Analyzer: spectrum after the EQ tap");
    analyzerPostButton.setClickAction([this] { setAnalyzerMode(kAnalyzerPost); });
    Component::addAndMakeVisible(analyzerPostButton);
    analyzerOffButton.setTooltip("Analyzer: hide the spectrum");
    analyzerOffButton.setClickAction([this] { setAnalyzerMode(kAnalyzerOff); });
    Component::addAndMakeVisible(analyzerOffButton);
    analyzerLabel.setJustificationType(juce::Justification::centredRight);
    analyzerLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    analyzerLabel.setColour(juce::Label::textColourId, theme::dim());
    Component::addAndMakeVisible(analyzerLabel);
    // RTA display offset (Task 6 follow-up): +/- buttons shift the analyzer
    // fill up/down in the view so quiet program material can be lifted into
    // the +/-15 dB window. Session-only; the engine never sees it.
    rtaMinusButton.setTooltip("RTA display: shift the analyzer down");
    rtaMinusButton.setClickAction([this] {
        setRtaOffsetDb(curve.getSpectrumOffsetDb() - kRtaOffsetStepDb);
    });
    Component::addAndMakeVisible(rtaMinusButton);
    rtaPlusButton.setTooltip("RTA display: shift the analyzer up");
    rtaPlusButton.setClickAction([this] {
        setRtaOffsetDb(curve.getSpectrumOffsetDb() + kRtaOffsetStepDb);
    });
    Component::addAndMakeVisible(rtaPlusButton);
    rtaOffsetLabel.setJustificationType(juce::Justification::centred);
    rtaOffsetLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    rtaOffsetLabel.setColour(juce::Label::textColourId, theme::dim());
    rtaOffsetLabel.setTooltip("RTA display offset in dB (view only)");
    Component::addAndMakeVisible(rtaOffsetLabel);
    setRtaOffsetDb(kDefaultRtaOffsetDb);
    // Analyzer on by default (Pre): the RTA is the reason the window has a
    // spectrum at all, and the tap writes only while a window is open.
    // Routes through setAnalyzerMode so the UI timer actually starts.
    setAnalyzerMode(kAnalyzerPre);

    centreWithSize(kWidth, kHeight);
    setVisible(true);
}

EQWindow::~EQWindow() {
    stopTimer();
    if (setAnalyzerTapMode) setAnalyzerTapMode(kAnalyzerOff); // silence the tap
}

void EQWindow::setAnalyzerMode(int mode) {
    if (mode == analyzerMode) return;
    analyzerMode = mode;
    if (setAnalyzerTapMode) setAnalyzerTapMode(mode);
    analyzerPreButton.setToggleState(mode == kAnalyzerPre);
    analyzerPostButton.setToggleState(mode == kAnalyzerPost);
    analyzerOffButton.setToggleState(mode == kAnalyzerOff);
    if (mode == kAnalyzerOff) {
        stopTimer();
        curve.setSpectrum(nullptr, 0); // Off hides the grey fill
        return;
    }
    startTimerHz(reducedToFps30 ? 30 : 60); // spec: 30-60 fps; timerCallback slows to 30 on overload
}

void EQWindow::setRtaOffsetDb(float offsetDb) {
    const float clamped = juce::jlimit(kRtaOffsetMinDb, kRtaOffsetMaxDb, offsetDb);
    curve.setSpectrumOffsetDb(clamped);
    const float shown = curve.getSpectrumOffsetDb();
    rtaOffsetLabel.setText((shown >= 0.0f ? "+" : "") + juce::String(static_cast<int>(shown)),
                           juce::dontSendNotification);
}

void EQWindow::timerCallback() {
    const auto startTicks = juce::Time::getHighResolutionTicks();

    // Pull the ring copy (a plain memcpy out of preallocated engine memory)
    // and mix down to mono through the Hann window. The back half of the
    // scratch is FFT workspace; only the first N floats are input.
    if (pullAnalyzerTap) {
        pullAnalyzerTap(analyzerMode == kAnalyzerPost,
                        tapL.data(), tapR.data());
    }
    for (int i = 0; i < kAnalyzerFftSize; ++i) {
        const std::size_t idx = static_cast<std::size_t>(i);
        fftScratch[idx] = (tapL[idx] + tapR[idx]) * 0.5f * hannCoeffs[idx];
    }
    analyzerFft->performFrequencyOnlyForwardTransform(fftScratch.data(),
                                                      true /* non-negative bins only */);

    // Bin magnitudes in dBFS. Normalisation: 2/N for the real-signal single-
    // sided peak amplitude, 1/0.5 for the Hann coherent gain. Silence floors
    // at -100 dB so the fill rests at the bottom of the view.
    constexpr float kNorm = 4.0f / static_cast<float>(kAnalyzerFftSize);
    for (std::size_t bin = 0; bin < spectrumDb.size(); ++bin) {
        const float mag = fftScratch[bin] * kNorm;
        spectrumDb[bin] = juce::jmax(-100.0f, 20.0f * std::log10(juce::jmax(1e-5f, mag)));
    }
    curve.setSpectrum(spectrumDb.data(), static_cast<int>(spectrumDb.size()));

    // Overload auto-drop (spec: never drops audio, only frames): if the pull
    // + FFT alone costs more than half the 30 fps budget five ticks running,
    // settle at 30 fps for the rest of this window's life.
    const double spent = juce::Time::highResolutionTicksToSeconds(
        juce::Time::getHighResolutionTicks() - startTicks);
    if (!reducedToFps30 && spent > 1.0 / 60.0) {
        if (++slowTicks >= 5) {
            reducedToFps30 = true;
            startTimerHz(30); // re-targets the running timer's fps
        }
    }
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
    auto analyzerRow = area.removeFromTop(20);
    analyzerLabel.setBounds(analyzerRow.removeFromLeft(60));
    analyzerRow.removeFromLeft(4);
    for (MiniButton* button : { &analyzerPreButton, &analyzerPostButton, &analyzerOffButton }) {
        button->setBounds(analyzerRow.removeFromLeft(44).reduced(2, 1));
        analyzerRow.removeFromLeft(2);
    }
    // RTA display offset, right-aligned: value label flanked by -/+ steppers.
    rtaPlusButton.setBounds(analyzerRow.removeFromRight(28).reduced(1, 1));
    analyzerRow.removeFromRight(2);
    rtaOffsetLabel.setBounds(analyzerRow.removeFromRight(34));
    analyzerRow.removeFromRight(2);
    rtaMinusButton.setBounds(analyzerRow.removeFromRight(28).reduced(1, 1));
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
