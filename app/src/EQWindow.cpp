#include "EQWindow.h"

#include <cmath>

EQWindow::EQWindow(const juce::String& title,
                   const std::vector<pluginrack::EqBand>& initialBands,
                   std::function<void(const std::vector<pluginrack::EqBand>&)> applyBandsCallback,
                   double sampleRateHz,
                   bool embeddedMode,
                   float initialRtaOffsetDb,
                   int initialWeighting)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      applyBands(std::move(applyBandsCallback)),
      embedded(embeddedMode) {
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    // A draggable window turns every press on the panel's background into a
    // window move: clicking near the curve's edge threw the editor across the
    // screen instead of editing. The title bar still moves it.
    setDraggable(false);
    // Width floor leaves room for the four knob slots (Freq/Gain/Q/Delay)
    // with their 72 px value boxes intact.
    setResizeLimits(480, 320, 10000, 10000);
    setColour(juce::DocumentWindow::backgroundColourId, theme::bg());

    curve.setSampleRateHz(sampleRateHz); // live rate before the first paint
    // Scenes written before the EQ gained its low-pass point carry six bands;
    // pad with the factory points so the HPF/LPF switches always have a point
    // to drive (and so the curve always shows the full set).
    {
        auto bands = initialBands;
        const auto defaults = pluginrack::defaultEqBands();
        for (std::size_t i = bands.size(); i < defaults.size(); ++i)
            bands.push_back(defaults[i]);
        curve.setBands(bands);
    }
    curve.onBandsChanged = [this] {
        if (applyBands) applyBands(curve.getBands());
        syncControls(); // node drag moved freq/gain: keep knobs in step
    };
    curve.onBandSelected = [this] { syncControls(); };
    // Intentional direct children (no content component): the sanctioned
    // base-class call documented by ResizableWindow's shadowing overload.
    Component::addAndMakeVisible(curve);

    stereoTag.setJustificationType(juce::Justification::centred);
    stereoTag.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    stereoTag.setColour(juce::Label::textColourId, theme::dim());
    stereoTag.setTooltip("Stereo processing - visual only in v1");
    Component::addAndMakeVisible(stereoTag);
    bypassButton.setFontSize(9.0f);
    soloButton.setFontSize(9.0f);
    flipButton.setFontSize(9.0f);
    flipButton.setTooltip("Flip phase (polarity) of the EQ output");
    flipButton.setClickAction([this] {
        const bool next = !flipButton.getToggleState();
        flipButton.setToggleState(next);
        if (applyPhaseFlip) applyPhaseFlip(next);
    });
    Component::addAndMakeVisible(flipButton);

    bypassButton.setTooltip("Bypass the selected band");
    bypassButton.setClickAction([this] {
        auto bands = curve.getBands();
        const int sel = curve.getSelectedBand();
        if (sel < 0 || sel >= static_cast<int>(bands.size())) return;
        bands[static_cast<std::size_t>(sel)].enabled = !bands[static_cast<std::size_t>(sel)].enabled;
        commitBands(std::move(bands));
    });
    Component::addAndMakeVisible(bypassButton);

    soloButton.setTooltip("Solo the selected band (audible bands: solo + enabled)");
    soloButton.setClickAction([this] {
        auto bands = curve.getBands();
        const int sel = curve.getSelectedBand();
        if (sel < 0 || sel >= static_cast<int>(bands.size())) return;
        bands[static_cast<std::size_t>(sel)].solo = !bands[static_cast<std::size_t>(sel)].solo;
        commitBands(std::move(bands));
    });
    Component::addAndMakeVisible(soloButton);

    // Bottom-left switches: the selected point's mode (bell vs shelf) and the
    // two filter points. The HPF/LPF switches force their point's type, so a
    // stray toggle can never leave the outer points without a filter.
    bandTypeButton.setFontSize(9.0f);
    bandTypeButton.setTooltip("Switch the selected point between bell (band) and shelf");
    bandTypeButton.setClickAction([this] { toggleBandType(); });
    Component::addAndMakeVisible(bandTypeButton);
    hpfButton.setFontSize(9.0f);
    hpfButton.setTooltip("Enable or bypass the low-cut (high-pass) point");
    hpfButton.setClickAction([this] { toggleFilterBand(0); });
    Component::addAndMakeVisible(hpfButton);
    lpfButton.setFontSize(9.0f);
    lpfButton.setTooltip("Enable or bypass the high-cut (low-pass) point");
    lpfButton.setClickAction([this] { toggleFilterBand(kLowPassBandIndex); });
    Component::addAndMakeVisible(lpfButton);

    for (auto* label : { &freqLabel, &gainLabel, &qLabel, &delayLabel }) {
        label->setJustificationType(juce::Justification::centred);
        label->setFont(juce::Font(juce::FontOptions(11.0f)));
        label->setColour(juce::Label::textColourId, theme::dim());
        Component::addAndMakeVisible(*label);
    }

    // Freq/Gain/Q knobs for the selected band, styled like the strip knobs
    // (ConsoleLookAndFeel conventions: control box, themed arcs).
    freqKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    freqKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 20);
    freqKnob.setRange(EQCurveComponent::kMinFreqHz, EQCurveComponent::kMaxFreqHz);
    // Double-click (and the rack strip's reset) return the point to 1 kHz.
    freqKnob.setDoubleClickReturnValue(true, 1000.0);
    freqKnob.setSkewFactorFromMidPoint(1000.0);
    freqKnob.textFromValueFunction = [](double hz) {
        return hz >= 1000.0 ? juce::String(hz / 1000.0, 2) + " kHz"
                            : juce::String(hz, 0) + " Hz";
    };
    freqKnob.valueFromTextFunction = [](const juce::String& text) {
        // The box shows "1.20 kHz", so accept both spellings of the unit.
        const auto trimmed = text.trim().toLowerCase();
        double hz = trimmed.getDoubleValue();
        if (trimmed.contains("khz") || trimmed.endsWithChar('k')) hz *= 1000.0;
        return juce::jlimit(static_cast<double>(EQCurveComponent::kMinFreqHz),
                            static_cast<double>(EQCurveComponent::kMaxFreqHz), hz);
    };
    freqKnob.onValueChange = [this] { applyFromControls(); };
    freqKnob.setColour(juce::Slider::rotarySliderFillColourId, theme::cyan());
    freqKnob.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
    freqKnob.setColour(juce::Slider::textBoxTextColourId, theme::text());
    freqKnob.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    freqKnob.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    Component::addAndMakeVisible(freqKnob);
    freqKnob.setName("Freq");

    gainKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    gainKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 20);
    gainKnob.setRange(-EQCurveComponent::kMaxGainDb, EQCurveComponent::kMaxGainDb, 0.1);
    gainKnob.setTextValueSuffix(" dB");
    gainKnob.setDoubleClickReturnValue(true, 0.0);
    gainKnob.onValueChange = [this] { applyFromControls(); };
    gainKnob.setColour(juce::Slider::rotarySliderFillColourId, theme::green());
    gainKnob.setColour(juce::Slider::rotarySliderOutlineColourId, theme::deepGreen());
    gainKnob.setColour(juce::Slider::textBoxTextColourId, theme::text());
    gainKnob.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    gainKnob.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    Component::addAndMakeVisible(gainKnob);
    gainKnob.setName("Gain");

    qKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    qKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 20);
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
    qKnob.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    Component::addAndMakeVisible(qKnob);
    qKnob.setName("Q");

    // Per-EQ delay (not per band): 0-1000 ms with a skew that gives the
    // short, alignment-relevant range most of the rotation. Editing engages
    // the EQ through the owner, exactly like a band edit does.
    delayKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    delayKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 20);
    delayKnob.setRange(0.0, static_cast<double>(pluginrack::EqEngine::kMaxDelayMs), 0.1);
    delayKnob.setSkewFactorFromMidPoint(100.0);
    delayKnob.textFromValueFunction = [](double ms) {
        return juce::String(ms, ms < 10.0 ? 1 : 0) + " ms";
    };
    delayKnob.valueFromTextFunction = [](const juce::String& text) {
        return juce::jlimit(0.0, static_cast<double>(pluginrack::EqEngine::kMaxDelayMs),
                            text.getDoubleValue());
    };
    delayKnob.setDoubleClickReturnValue(true, 0.0);
    delayKnob.setTooltip("Post-EQ delay in ms (0-1000)");
    delayKnob.onValueChange = [this] { applyDelayFromControl(); };
    delayKnob.setColour(juce::Slider::rotarySliderFillColourId, theme::amber());
    delayKnob.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
    delayKnob.setColour(juce::Slider::textBoxTextColourId, theme::text());
    delayKnob.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    delayKnob.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    Component::addAndMakeVisible(delayKnob);
    delayKnob.setName("Delay");

    syncControls();

    // --- analyzer (Task 6): one FFT per open window, everything allocated
    // here on the message thread so the timer callback never touches the
    // heap. A Hann window keeps spectral leakage down; the tap rings arrive
    // through pullAnalyzerTap, wired by the owning strip.
    analyzerFft = std::make_unique<juce::dsp::FFT>(13); // 2^13 = 8192
    hannCoeffs.resize(static_cast<std::size_t>(kAnalyzerFftSize));
    for (int i = 0; i < kAnalyzerFftSize; ++i) {
        const double t = juce::MathConstants<double>::twoPi * i / kAnalyzerFftSize;
        hannCoeffs[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(t));
    }
    fftScratch.assign(static_cast<std::size_t>(kAnalyzerFftSize) * 2, 0.0f);
    tapL.assign(static_cast<std::size_t>(kAnalyzerFftSize), 0.0f);
    tapR.assign(static_cast<std::size_t>(kAnalyzerFftSize), 0.0f);
    spectrumDb.assign(static_cast<std::size_t>(kAnalyzerFftSize) / 2 + 1, 0.0f);
    spectrumAvg.assign(static_cast<std::size_t>(kAnalyzerFftSize) / 2 + 1, -100.0f);
    // Analyzer mode is fixed to Post (no Pre/Post/Off switch): the window
    // always shows the post-EQ spectrum. The owning strip starts it via
    // setAnalyzerMode after wiring the tap callbacks, because the ctor runs
    // before those callbacks exist and starting it here would address no one
    // (this was the "dead RTA until mode toggle" bug).
    analyzerLabel.setJustificationType(juce::Justification::centredRight);
    analyzerLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    analyzerLabel.setColour(juce::Label::textColourId, theme::dim());
    Component::addAndMakeVisible(analyzerLabel);
    // RTA display offset: +/- buttons shift the analyzer fill up/down in the
    // view so quiet program material can be lifted into the +/-15 dB window.
    // The value is per strip and saved with the scene; 0 is the default view.
    rtaMinusButton.setTooltip("RTA display: shift the analyzer down");
    rtaMinusButton.setClickAction([this] {
        setRtaOffsetDb(rtaOffsetDb - kRtaOffsetStepDb);
    });
    Component::addAndMakeVisible(rtaMinusButton);
    rtaPlusButton.setTooltip("RTA display: shift the analyzer up");
    rtaPlusButton.setClickAction([this] {
        setRtaOffsetDb(rtaOffsetDb + kRtaOffsetStepDb);
    });
    Component::addAndMakeVisible(rtaPlusButton);
    rtaOffsetLabel.setJustificationType(juce::Justification::centred);
    rtaOffsetLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    rtaOffsetLabel.setColour(juce::Label::textColourId, theme::dim());
    rtaOffsetLabel.setTooltip("RTA display offset in dB, saved with the scene");
    Component::addAndMakeVisible(rtaOffsetLabel);

    // Analyzer weighting: three radio-style buttons, one active at a time.
    // Display curve only, so the EQ response and the audio path are untouched.
    const char* weightingTips[3] = {
        "Z: flat - the spectrum as measured",
        "A: A-weighting (IEC 61672)",
        "C: C-weighting (IEC 61672)",
    };
    for (int i = 0; i < 3; ++i) {
        auto* button = weightingButtons[static_cast<std::size_t>(i)];
        button->setTooltip(weightingTips[i]);
        button->setFontSize(9.0f);
        button->setClickAction([this, i] { setWeightingIndex(i); });
        Component::addAndMakeVisible(*button);
    }
    setWeightingIndex(initialWeighting);

    setRtaOffsetDb(initialRtaOffsetDb);

    if (!embedded) {
        centreWithSize(kWidth, kHeight);
        setVisible(true);
    } else {
        setSize(kWidth, kHeight);
    }
}

EQWindow::~EQWindow() {
    stopTimer();
    if (setAnalyzerTapMode) setAnalyzerTapMode(kAnalyzerOff); // silence the tap
}

void EQWindow::setAnalyzerMode(int mode) {
    // Fixed to Post by the owners (no mode switch in the UI): the strip
    // calls this once after wiring the tap callbacks. Kept as a function
    // (rather than inlining) so the start/stop/tap-gating stays in one place.
    if (mode == analyzerMode) return;
    analyzerMode = mode;
    if (setAnalyzerTapMode) setAnalyzerTapMode(mode);
    if (mode == kAnalyzerOff) {
        stopTimer();
        curve.setSpectrum(nullptr, 0); // Off hides the grey fill
        return;
    }
    startTimerHz(reducedToFps30 ? 30 : 60); // spec: 30-60 fps; timerCallback slows to 30 on overload
}

void EQWindow::setWeightingIndex(int index) {
    const int clamped = juce::jlimit(0, 2, index);
    const bool changed = clamped != weightingIndex;
    weightingIndex = clamped;
    for (int i = 0; i < 3; ++i)
        weightingButtons[static_cast<std::size_t>(i)]->setToggleState(i == clamped);
    if (!changed) return;
    curve.setWeighting(EQCurveComponent::weightingFromIndex(clamped));
    // Empty during construction: owners wire this after the window exists.
    if (applyWeighting) applyWeighting(clamped);
}

void EQWindow::setRtaOffsetDb(float offsetDb) {
    const float clamped = juce::jlimit(kRtaOffsetMinDb, kRtaOffsetMaxDb, offsetDb);
    rtaOffsetDb = clamped;
    // The saved value is relative to the default view, so 0 shows the same
    // analyzer height as before the offset existed.
    curve.setSpectrumOffsetDb(EQCurveComponent::kDefaultSpectrumOffsetDb + clamped);
    // 0 is the default view, so it reads as plain "0" rather than "+0".
    const int shown = juce::roundToInt(clamped);
    rtaOffsetLabel.setText((shown > 0 ? "+" : "") + juce::String(shown),
                           juce::dontSendNotification);
    // Empty during construction: owners wire this after the window exists.
    if (applySpectrumOffset) applySpectrumOffset(clamped);
}

void EQWindow::timerCallback() {
    const auto startTicks = juce::Time::getHighResolutionTicks();

    // Pull the ring copy (a plain memcpy out of preallocated engine memory)
    // and mix down to mono through the Hann window. The back half of the
    // scratch is FFT workspace; only the first N floats are input.
    if (pullAnalyzerTap) {
        // Re-arm the shared tap: closing a dynamics panel elsewhere switches it off.
        if (setAnalyzerTapMode && analyzerMode != kAnalyzerOff)
            setAnalyzerTapMode(analyzerMode);
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
    // Pro-Q-style ballistics: fast attack so onsets register instantly, slow
    // release so the trace integrates instead of flickering. The smoothed
    // copy is what the curve draws, so a -6 dBFS tone still lands on the
    // -6 dB gridline while broadband music reads as a steady spectrum.
    for (std::size_t bin = 0; bin < spectrumDb.size(); ++bin) {
        const float fresh = spectrumDb[bin];
        float& held = spectrumAvg[bin];
        held += (fresh - held) * (fresh > held ? 0.6f : 0.15f);
    }
    curve.setSpectrum(spectrumAvg.data(), static_cast<int>(spectrumAvg.size()));

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

void EQWindow::applyDelayFromControl() {
    if (applyDelayMs) applyDelayMs(static_cast<float>(delayKnob.getValue()));
}

void EQWindow::commitBands(std::vector<pluginrack::EqBand> bands) {
    curve.setBands(bands);
    syncControls();
    if (applyBands) applyBands(curve.getBands());
}

void EQWindow::toggleBandType() {
    auto bands = curve.getBands();
    const int sel = curve.getSelectedBand();
    if (sel < 0 || sel >= static_cast<int>(bands.size())) return;
    pluginrack::EqBand& band = bands[static_cast<std::size_t>(sel)];
    if (band.type == pluginrack::EqType::HighPass || band.type == pluginrack::EqType::LowPass)
        return; // filter points: the HPF/LPF switches own their mode
    if (band.type == pluginrack::EqType::Peak) {
        // The shelf shapes whichever end the point sits on: below the middle of
        // the spectrum a low shelf, above it a high shelf.
        band.type = band.freqHz < 1000.0f ? pluginrack::EqType::LowShelf
                                          : pluginrack::EqType::HighShelf;
    } else {
        band.type = pluginrack::EqType::Peak;
    }
    commitBands(std::move(bands));
}

void EQWindow::toggleFilterBand(int index) {
    auto bands = curve.getBands();
    if (index < 0 || index >= static_cast<int>(bands.size())) return;
    pluginrack::EqBand& band = bands[static_cast<std::size_t>(index)];
    band.type = index == 0 ? pluginrack::EqType::HighPass : pluginrack::EqType::LowPass;
    band.enabled = !band.enabled;
    band.gainDb = 0.0f; // filter points ignore gain: keep the node on 0 dB
    commitBands(std::move(bands));
}

void EQWindow::setDelayMs(float delayMs) {
    delayKnob.setValue(juce::jlimit(0.0, static_cast<double>(pluginrack::EqEngine::kMaxDelayMs),
                                    static_cast<double>(delayMs)),
                       juce::dontSendNotification);
}

void EQWindow::setPhaseFlip(bool flipped) {
    flipButton.setToggleState(flipped);
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

    // High-pass and low-pass ignore gain and Q in the engine; disable knobs
    // that would do nothing so the UI never lies about its effect.
    const bool isHighPass = (band.type == pluginrack::EqType::HighPass);
    const bool isLowPass = (band.type == pluginrack::EqType::LowPass);
    const bool isFilterPoint = isHighPass || isLowPass;
    gainKnob.setEnabled(!isFilterPoint);
    qKnob.setEnabled(!isFilterPoint);

    // Point mode: a bell reads "BAND", a shelf reads "SHELF". The filter
    // points are not switchable here (their type would stop being a filter).
    const bool isShelf = band.type == pluginrack::EqType::LowShelf
                      || band.type == pluginrack::EqType::HighShelf;
    bandTypeButton.setLabel(isShelf ? "SHELF" : "BAND");
    bandTypeButton.setToggleState(isShelf);
    bandTypeButton.setEnabledState(!isFilterPoint);

    // Filter switches always reflect the whole set, not the selection.
    const auto& all = curve.getBands();
    if (!all.empty())
        hpfButton.setToggleState(all.front().type == pluginrack::EqType::HighPass
                                 && all.front().enabled);
    if (static_cast<int>(all.size()) > kLowPassBandIndex)
        lpfButton.setToggleState(all[static_cast<std::size_t>(kLowPassBandIndex)].type
                                     == pluginrack::EqType::LowPass
                                 && all[static_cast<std::size_t>(kLowPassBandIndex)].enabled);
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

void EQWindow::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
    auto content = getLocalBounds().reduced(2);
    g.setColour(theme::panel().darker(0.04f));
    g.fillRect(content.removeFromTop(24));
}

void EQWindow::resized() {
    // With a native title bar the component bounds *are* the content area.
    auto area = getLocalBounds().reduced(2);
    // Tall enough for a label, the dial and its value box without clipping the
    // box against the window edge.
    auto controlsRow = area.removeFromBottom(120);
    auto analyzerRow = area.removeFromTop(24);
    analyzerLabel.setBounds(analyzerRow.removeFromLeft(60));
    analyzerRow.removeFromLeft(4);
    // (Mode buttons removed: analyzer is fixed to Post. The freed space
    // stays empty; the offset steppers are right-aligned below.)
    // RTA display offset, right-aligned: value label flanked by -/+ steppers.
    rtaPlusButton.setBounds(analyzerRow.removeFromRight(28).reduced(1, 1));
    analyzerRow.removeFromRight(2);
    rtaOffsetLabel.setBounds(analyzerRow.removeFromRight(34));
    analyzerRow.removeFromRight(2);
    rtaMinusButton.setBounds(analyzerRow.removeFromRight(28).reduced(1, 1));
    analyzerRow.removeFromRight(6);
    // Radio group, laid out right-to-left so it reads Z A C left to right.
    for (int i = 2; i >= 0; --i) {
        weightingButtons[static_cast<std::size_t>(i)]
            ->setBounds(analyzerRow.removeFromRight(24).reduced(1, 1));
        analyzerRow.removeFromRight(2);
    }
    area.removeFromTop(6); // air between the analyzer row and the dB scale
    curve.setBounds(area);

    auto left = controlsRow.removeFromLeft(84);
    left.removeFromTop(6);
    left.removeFromLeft(6);
    bandTypeButton.setBounds(left.removeFromTop(24));
    left.removeFromTop(4);
    hpfButton.setBounds(left.removeFromTop(22));
    left.removeFromTop(4);
    lpfButton.setBounds(left.removeFromTop(22));
    left.removeFromTop(4);
    stereoTag.setBounds(left.removeFromTop(14));
    controlsRow.removeFromLeft(4);

    auto right = controlsRow.removeFromRight(80);
    right.removeFromTop(8);
    right.removeFromLeft(6);
    bypassButton.setBounds(right.removeFromTop(22));
    right.removeFromTop(4);
    soloButton.setBounds(right.removeFromTop(22));
    right.removeFromTop(4);
    flipButton.setBounds(right.removeFromTop(22));

    // Four label+knob slots across the remaining middle (bands, then delay).
    const int knobW = juce::jmax(1, controlsRow.getWidth() / 4);
    auto slot = [&](int i) {
        return juce::Rectangle<int>(controlsRow.getX() + i * knobW, controlsRow.getY(),
                                    knobW, controlsRow.getHeight());
    };
    for (int i = 0; i < 4; ++i) {
        auto s = slot(i);
        auto* label = i == 0 ? &freqLabel
                             : (i == 1 ? &gainLabel : (i == 2 ? &qLabel : &delayLabel));
        auto* knob = i == 0 ? &freqKnob
                            : (i == 1 ? &gainKnob : (i == 2 ? &qKnob : &delayKnob));
        label->setBounds(s.removeFromTop(14));
        auto knobArea = s.withTrimmedBottom(6);
        const int knobWidth = juce::jmin(knobArea.getWidth(),
                                         juce::jmax(66, juce::roundToInt(knobArea.getWidth() * 0.8f)));
        knob->setBounds(knobArea.withSizeKeepingCentre(knobWidth, knobArea.getHeight()));
    }
}
