#include "SpectralDenoiserWindow.h"

#include "AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPlotFloorDb = -80.0f;
constexpr float kPlotCeilingDb = 6.0f;
constexpr float kMeterHeight = 64.0f;
constexpr float kMeterGap = 10.0f;
constexpr float kMeterFloorDb = 40.0f;

} // namespace

SpectralDenoiserProfileView::SpectralDenoiserProfileView(
    pluginrack::SpectralDenoiserEffect& effectToEdit)
    : effect(effectToEdit) {
    setInterceptsMouseClicks(true, false);
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

void SpectralDenoiserProfileView::setReduction(
    const std::array<float, pluginrack::kSpectralDenoiserTrimBandCount>& bands) {
    float change = 0.0f;
    for (std::size_t band = 0; band < bands.size(); ++band) {
        change = std::max(change, std::abs(bands[band] - reductionDb[band]));
    }
    reductionDb = bands;
    if (change >= 0.05f) {
        repaint();
    }
}

void SpectralDenoiserProfileView::ensureTrims() {
    if (effect.profileTrimDb.size()
        != static_cast<std::size_t>(pluginrack::kSpectralDenoiserTrimBandCount)) {
        effect.profileTrimDb.assign(pluginrack::kSpectralDenoiserTrimBandCount, 0.0f);
    }
}

juce::Rectangle<float> SpectralDenoiserProfileView::plotArea() const {
    auto area = getLocalBounds().toFloat().reduced(8.0f, 20.0f);
    area.removeFromBottom(kMeterHeight + kMeterGap);
    return area;
}

juce::Rectangle<float> SpectralDenoiserProfileView::meterArea() const {
    auto area = getLocalBounds().toFloat().reduced(8.0f, 20.0f);
    return area.removeFromBottom(kMeterHeight);
}

float SpectralDenoiserProfileView::frequencyForBin(std::size_t bin) const {
    const double rate = effect.profileSampleRate > 0.0 ? effect.profileSampleRate : 48000.0;
    return static_cast<float>(static_cast<double>(bin) * rate * 0.5
                              / static_cast<double>(pluginrack::kSpectralDenoiserBinCount - 1));
}

float SpectralDenoiserProfileView::frequencyForBandCentre(int band) const {
    const double rate = effect.profileSampleRate > 0.0 ? effect.profileSampleRate : 48000.0;
    const double nyquist = rate * 0.5;
    const double low = pluginrack::kSpectralDenoiserTrimLowHz;
    const double position = (static_cast<double>(band) + 0.5)
        / static_cast<double>(pluginrack::kSpectralDenoiserTrimBandCount);
    return static_cast<float>(low * std::pow(nyquist / low, position));
}

float SpectralDenoiserProfileView::xForFrequency(float hz) const {
    const auto plot = plotArea();
    const double rate = effect.profileSampleRate > 0.0 ? effect.profileSampleRate : 48000.0;
    const double low = pluginrack::kSpectralDenoiserTrimLowHz;
    const double nyquist = rate * 0.5;
    if (!(hz > low) || !(nyquist > low)) {
        return plot.getX();
    }
    const double position = std::log(static_cast<double>(hz) / low) / std::log(nyquist / low);
    return plot.getX() + static_cast<float>(juce::jlimit(0.0, 1.0, position)) * plot.getWidth();
}

int SpectralDenoiserProfileView::bandAt(int x) const {
    const auto plot = plotArea();
    const double rate = effect.profileSampleRate > 0.0 ? effect.profileSampleRate : 48000.0;
    const double low = pluginrack::kSpectralDenoiserTrimLowHz;
    const double nyquist = rate * 0.5;
    const double position = plot.getWidth() > 0.0f
        ? juce::jlimit(0.0, 1.0, static_cast<double>(x - plot.getX()) / plot.getWidth())
        : 0.0;
    const double frequency = low * std::pow(nyquist / low, position);
    return pluginrack::spectralDenoiserTrimBand(frequency, nyquist);
}

float SpectralDenoiserProfileView::trimPixelsPerDb() const {
    return std::max(plotArea().getHeight() / 36.0f, 1.0f);
}

void SpectralDenoiserProfileView::paint(juce::Graphics& g) {
    auto plot = plotArea();
    g.setColour(theme::control());
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
    g.setColour(theme::border());
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.0f, 1.0f);

    g.setColour(theme::line().withAlpha(0.35f));
    const double nyquist = (effect.profileSampleRate > 0.0 ? effect.profileSampleRate : 48000.0) * 0.5;
    for (const double hz : {100.0, 1000.0, 10000.0}) {
        if (hz >= nyquist) {
            continue;
        }
        const float x = xForFrequency(static_cast<float>(hz));
        g.drawVerticalLine(static_cast<int>(std::lround(x)),
                           plot.getY(), plot.getBottom());
    }
    for (int i = 1; i < 4; ++i) {
        const float y = plot.getY() + plot.getHeight() * static_cast<float>(i) / 4.0f;
        g.drawHorizontalLine(static_cast<int>(std::lround(y)), plot.getX(), plot.getRight());
    }

    const bool hasProfile = effect.hasProfile();
    const float reference = hasProfile
        ? *std::max_element(effect.noiseProfile.begin(), effect.noiseProfile.end())
        : 0.0f;

    auto yForDb = [&plot](float db) {
        const float clamped = juce::jlimit(kPlotFloorDb, kPlotCeilingDb, db);
        const float position = (clamped - kPlotFloorDb) / (kPlotCeilingDb - kPlotFloorDb);
        return plot.getBottom() - position * plot.getHeight();
    };

    if (hasProfile && reference > 0.0f) {
        juce::Path profilePath;
        for (std::size_t bin = 0; bin < effect.noiseProfile.size(); ++bin) {
            const float db = 10.0f * std::log10(
                std::max(effect.noiseProfile[bin], 1.0e-12f) / reference);
            const float x = xForFrequency(frequencyForBin(bin));
            const float y = yForDb(db);
            if (bin == 0) profilePath.startNewSubPath(x, y);
            else profilePath.lineTo(x, y);
        }
        g.setColour(theme::cyan());
        g.strokePath(profilePath, juce::PathStrokeType(1.6f));
    }

    // Threshold after the band trims, plus a handle per band.
    const int trimCount = pluginrack::kSpectralDenoiserTrimBandCount;
    auto bandBaseDb = [&](int band) {
        if (!hasProfile || reference <= 0.0f) {
            return -24.0f;
        }
        const float binWidth = frequencyForBin(1) - frequencyForBin(0);
        const std::size_t centreBin = static_cast<std::size_t>(std::clamp(
            static_cast<int>(std::lround(frequencyForBandCentre(band) / binWidth)),
            0, pluginrack::kSpectralDenoiserBinCount - 1));
        return 10.0f * std::log10(
            std::max(effect.noiseProfile[centreBin], 1.0e-12f) / reference);
    };

    juce::Path trimPath;
    for (int band = 0; band < trimCount; ++band) {
        const float trim = pluginrack::spectralDenoiserTrimDb(effect.profileTrimDb, band);
        const float x = xForFrequency(frequencyForBandCentre(band));
        const float y = yForDb(bandBaseDb(band) + trim);
        if (band == 0) trimPath.startNewSubPath(x, y);
        else trimPath.lineTo(x, y);
    }
    g.setColour(theme::amber().withAlpha(0.9f));
    g.strokePath(trimPath, juce::PathStrokeType(1.4f));

    for (int band = 0; band < trimCount; ++band) {
        const float trim = pluginrack::spectralDenoiserTrimDb(effect.profileTrimDb, band);
        const float x = xForFrequency(frequencyForBandCentre(band));
        const float y = yForDb(bandBaseDb(band) + trim);
        g.setColour(band == dragBand ? theme::text() : theme::orange());
        g.fillEllipse(x - 2.5f, y - 2.5f, 5.0f, 5.0f);
    }

    if (!hasProfile) {
        g.setColour(theme::dim());
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText("Press Learn to capture a noise profile",
                   getLocalBounds().reduced(8), juce::Justification::centred, false);
    } else {
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        const auto legend = getLocalBounds().reduced(12, 4).removeFromTop(12);
        g.setColour(theme::cyan());
        g.drawText("Noise profile", legend, juce::Justification::topLeft, false);
        g.setColour(theme::amber());
        g.drawText("After trim", legend.withTrimmedLeft(84),
                   juce::Justification::topLeft, false);
    }

    // Live gain reduction, one bar per trim band, with the envelope shaded so
    // the shape of the cut reads at a glance.
    const auto meter = meterArea();
    g.setColour(theme::control().darker(0.35f));
    g.fillRoundedRectangle(meter, 3.0f);
    g.setColour(theme::line().withAlpha(0.35f));
    g.drawRoundedRectangle(meter.reduced(0.5f), 3.0f, 1.0f);

    auto yForReduction = [&meter](float db) {
        const float position = juce::jlimit(0.0f, 1.0f, db / kMeterFloorDb);
        return meter.getBottom() - position * meter.getHeight();
    };
    for (const float db : {0.0f, 12.0f, 24.0f, 40.0f}) {
        g.setColour(theme::line().withAlpha(db == 0.0f ? 0.5f : 0.25f));
        g.drawHorizontalLine(static_cast<int>(std::lround(yForReduction(db))),
                             meter.getX(), meter.getRight());
    }

    const double trimLow = pluginrack::kSpectralDenoiserTrimLowHz;
    juce::Path envelope;
    std::array<float, pluginrack::kSpectralDenoiserTrimBandCount> centres{};
    for (int band = 0; band < pluginrack::kSpectralDenoiserTrimBandCount; ++band) {
        const double lowPosition = static_cast<double>(band)
            / pluginrack::kSpectralDenoiserTrimBandCount;
        const double highPosition = static_cast<double>(band + 1)
            / pluginrack::kSpectralDenoiserTrimBandCount;
        const float left = xForFrequency(
            static_cast<float>(trimLow * std::pow(nyquist / trimLow, lowPosition)));
        const float right = xForFrequency(
            static_cast<float>(trimLow * std::pow(nyquist / trimLow, highPosition)));
        const float x = 0.5f * (left + right);
        centres[static_cast<std::size_t>(band)] = x;
        const float value = reductionDb[static_cast<std::size_t>(band)];
        const float top = yForReduction(value);
        const float barWidth = std::max(right - left - 1.5f, 1.5f);
        g.setColour(theme::cyan().withAlpha(0.30f + 0.5f * (value / kMeterFloorDb)));
        g.fillRoundedRectangle(left + 0.75f, top,
                               barWidth, meter.getBottom() - top,
                               1.0f);
        if (band == 0) envelope.startNewSubPath(x, top);
        else envelope.lineTo(x, top);
    }

    if (effect.hasProfile() || effect.mode == pluginrack::SpectralDenoiserMode::Adaptive) {
        envelope.lineTo(centres.back(), meter.getBottom());
        envelope.lineTo(centres.front(), meter.getBottom());
        envelope.closeSubPath();
        g.setColour(theme::orange().withAlpha(0.16f));
        g.fillPath(envelope);
    }

    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.setColour(theme::dim());
    g.drawText("GR", meter.reduced(5, 2).removeFromTop(10),
               juce::Justification::topLeft, false);
    g.drawText("0", meter.reduced(5, 1).removeFromTop(10),
               juce::Justification::topRight, false);
    g.drawText("-40 dB", meter.reduced(5, 1).removeFromBottom(10),
               juce::Justification::bottomRight, false);
}

void SpectralDenoiserProfileView::mouseDown(const juce::MouseEvent& e) {
    dragBand = bandAt(e.x);
    ensureTrims();
    dragStartTrim = effect.profileTrimDb[static_cast<std::size_t>(dragBand)];
    dragStartY = e.y;
    repaint();
}

void SpectralDenoiserProfileView::mouseDrag(const juce::MouseEvent& e) {
    if (dragBand < 0) {
        return;
    }
    ensureTrims();
    const float delta = static_cast<float>(dragStartY - e.y) / trimPixelsPerDb();
    const float trimmed = juce::jlimit(pluginrack::kSpectralDenoiserMinTrimDb,
                                       pluginrack::kSpectralDenoiserMaxTrimDb,
                                       dragStartTrim + delta);
    const float snapped = std::round(trimmed * 2.0f) * 0.5f;
    effect.profileTrimDb[static_cast<std::size_t>(dragBand)] = snapped;
    repaint();
    if (onTrimEdited) {
        onTrimEdited();
    }
}

void SpectralDenoiserProfileView::mouseDoubleClick(const juce::MouseEvent& e) {
    const int band = bandAt(e.x);
    ensureTrims();
    effect.profileTrimDb[static_cast<std::size_t>(band)] = 0.0f;
    repaint();
    if (onTrimEdited) {
        onTrimEdited();
    }
}

void SpectralDenoiserWindow::setupKnob(ValueEditSlider& slider, juce::Label& label,
                                       const juce::String& name, double min, double max,
                                       double step, double value, double defaultValue,
                                       float pluginrack::SpectralDenoiserEffect::*field,
                                       const juce::String& suffix) {
    content.addAndMakeVisible(slider);
    content.addAndMakeVisible(label);
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 18);
    slider.setRange(min, max, step);
    slider.setTextValueSuffix(suffix);
    slider.setDoubleClickReturnValue(true, defaultValue);
    slider.setColour(juce::Slider::rotarySliderFillColourId, theme::cyan());
    slider.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
    slider.setColour(juce::Slider::textBoxTextColourId, theme::text());
    slider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    slider.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
    slider.setValue(value, juce::dontSendNotification);
    slider.onValueChange = [this, &slider, field] {
        effect.*field = static_cast<float>(slider.getValue());
        if (apply) apply(effect, false);
    };

    label.setText(name, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setFont(juce::Font(juce::FontOptions(11.0f)));
    label.setColour(juce::Label::textColourId, theme::dim());
}

SpectralDenoiserWindow::SpectralDenoiserWindow(const juce::String& title,
                                               const pluginrack::SpectralDenoiserEffect& initialEffect,
                                               AudioEngine& engine, bool isOutput,
                                               std::uint32_t stripId, ApplyCallback applyCallback)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      effect(initialEffect), apply(std::move(applyCallback)), audioEngine(engine),
      outputStrip(isOutput), targetId(stripId), profileView(effect) {
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

    theme::ui::styleButton(resetTrimButton);
    content.addAndMakeVisible(resetTrimButton);
    resetTrimButton.onClick = [this] {
        effect.profileTrimDb.clear();
        profileView.repaint();
        if (apply) apply(effect, false);
        refreshStatus();
    };

    theme::ui::styleButton(listenButton);
    content.addAndMakeVisible(listenButton);
    listenButton.setClickingTogglesState(true);
    listenButton.setToggleState(effect.listen, juce::dontSendNotification);
    listenButton.onClick = [this] {
        effect.listen = listenButton.getToggleState();
        if (apply) apply(effect, false);
        refreshStatus();
    };

    content.addAndMakeVisible(profileView);
    profileView.onTrimEdited = [this] {
        if (apply) apply(effect, false);
    };

    setupKnob(reduction, reductionLabel, "Max Reduction",
              pluginrack::kSpectralDenoiserMinReductionDb,
              pluginrack::kSpectralDenoiserMaxReductionDb, 0.5, effect.reductionDb,
              pluginrack::kSpectralDenoiserDefaultReductionDb,
              &pluginrack::SpectralDenoiserEffect::reductionDb, " dB");
    setupKnob(thresholdOffset, thresholdOffsetLabel, "Threshold",
              pluginrack::kSpectralDenoiserMinThresholdOffsetDb,
              pluginrack::kSpectralDenoiserMaxThresholdOffsetDb, 0.5, effect.thresholdOffsetDb,
              0.0, &pluginrack::SpectralDenoiserEffect::thresholdOffsetDb, " dB");
    setupKnob(mix, mixLabel, "Mix", pluginrack::kSpectralDenoiserMinMixPercent,
              pluginrack::kSpectralDenoiserMaxMixPercent, 1.0, effect.mixPercent,
              pluginrack::kSpectralDenoiserMaxMixPercent,
              &pluginrack::SpectralDenoiserEffect::mixPercent, " %");
    setupKnob(speed, speedLabel, "Speed", pluginrack::kSpectralDenoiserMinSpeed,
              pluginrack::kSpectralDenoiserMaxSpeed, 0.05, effect.speed, 1.0,
              &pluginrack::SpectralDenoiserEffect::speed, "x");
    setupKnob(stereoLink, stereoLinkLabel, "Stereo Link",
              pluginrack::kSpectralDenoiserMinStereoLinkPercent,
              pluginrack::kSpectralDenoiserMaxStereoLinkPercent, 1.0,
              effect.stereoLinkPercent, pluginrack::kSpectralDenoiserMaxStereoLinkPercent,
              &pluginrack::SpectralDenoiserEffect::stereoLinkPercent, " %");
    setupKnob(learnLength, learnLengthLabel, "Learn Time",
              pluginrack::kSpectralDenoiserMinLearnSeconds,
              pluginrack::kSpectralDenoiserMaxLearnSeconds, 0.5, effect.learnSeconds, 1.0,
              &pluginrack::SpectralDenoiserEffect::learnSeconds, " s");

    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    statusLabel.setColour(juce::Label::textColourId, theme::dim());
    content.addAndMakeVisible(statusLabel);

    refreshStatus();
    setVisible(true);
    startTimer(50);
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
    effect.profileTrimDb.clear();
    learning = false;
    profileView.repaint();
    if (apply) apply(effect, false);
    refreshStatus();
}

void SpectralDenoiserWindow::refreshStatus() {
    if (learning) return;
    const int trimmed = static_cast<int>(std::count_if(
        effect.profileTrimDb.begin(), effect.profileTrimDb.end(),
        [](float value) { return std::abs(value) >= 0.25f; }));
    const juce::String trimText = trimmed > 0
        ? " · " + juce::String(trimmed) + (trimmed == 1 ? " band" : " bands") + " trimmed"
        : juce::String();
    const juce::String offsetText = std::abs(effect.thresholdOffsetDb) >= 0.25f
        ? " · threshold " + juce::String(effect.thresholdOffsetDb > 0.0f ? "+" : "")
            + juce::String(effect.thresholdOffsetDb, 1) + " dB"
        : juce::String();
    const juce::String listenText = effect.listen ? " · listening to the removed signal" : "";
    if (effect.hasProfile()) {
        statusLabel.setText("Noise profile saved" + trimText + offsetText + listenText,
                            juce::dontSendNotification);
    } else if (effect.mode == pluginrack::SpectralDenoiserMode::Learn) {
        statusLabel.setText("Learn a noise-only profile to reduce noise" + offsetText + listenText,
                            juce::dontSendNotification);
    } else {
        statusLabel.setText("Adaptive: tracks the noise floor as it plays"
                                + trimText + offsetText + listenText,
                            juce::dontSendNotification);
    }
}

void SpectralDenoiserWindow::closeButtonPressed() {
    // Listen is a monitoring state, not a scene setting; leave the effect in
    // its normal mode if the editor is closed while it is engaged.
    if (effect.listen) {
        effect.listen = false;
        if (apply) apply(effect, false);
    }
    if (onClose) onClose();
}

void SpectralDenoiserWindow::timerCallback() {
    std::array<float, pluginrack::kSpectralDenoiserTrimBandCount> bands{};
    if (!effect.bypassed) {
        bands = outputStrip
            ? audioEngine.getOutputDenoiserBandReduction(targetId, effect.id)
            : audioEngine.getChannelDenoiserBandReduction(targetId, effect.id);
    }
    profileView.setReduction(bands);

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
        profileView.repaint();
        refreshStatus();
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
    modeCombo.setBounds(topRow.removeFromLeft(110));
    topRow.removeFromLeft(8);
    learnButton.setBounds(topRow.removeFromLeft(76));
    topRow.removeFromLeft(8);
    clearButton.setBounds(topRow.removeFromLeft(100));
    topRow.removeFromLeft(8);
    resetTrimButton.setBounds(topRow.removeFromLeft(96));
    topRow.removeFromLeft(8);
    listenButton.setBounds(topRow.removeFromLeft(84));

    area.removeFromTop(4);
    statusLabel.setBounds(area.removeFromTop(18));

    area.removeFromTop(6);
    const int knobRowHeight = juce::jlimit(110, 150, (area.getHeight() - 128) / 2);
    const int graphHeight = juce::jmax(120, area.getHeight() - knobRowHeight * 2 - 8);
    profileView.setBounds(area.removeFromTop(graphHeight));
    area.removeFromTop(8);

    const auto placeRow = [](juce::Rectangle<int> row,
                             std::initializer_list<std::pair<juce::Label*, ValueEditSlider*>> knobs) {
        const int cellWidth = row.getWidth() / static_cast<int>(knobs.size());
        for (const auto& [label, slider] : knobs) {
            auto cell = row.removeFromLeft(cellWidth);
            theme::ui::placeWindowKnob(
                cell.withSizeKeepingCentre(juce::jmin(150, cell.getWidth()), cell.getHeight()),
                *label, *slider);
        }
    };

    auto firstRow = area.removeFromTop(juce::jmin(knobRowHeight, area.getHeight()));
    placeRow(firstRow, {{&thresholdOffsetLabel, &thresholdOffset},
                        {&reductionLabel, &reduction},
                        {&mixLabel, &mix}});
    auto secondRow = area.removeFromTop(juce::jmin(knobRowHeight, area.getHeight()));
    placeRow(secondRow, {{&speedLabel, &speed},
                         {&stereoLinkLabel, &stereoLink},
                         {&learnLengthLabel, &learnLength}});
}
