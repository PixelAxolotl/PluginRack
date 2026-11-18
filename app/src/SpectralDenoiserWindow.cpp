#include "SpectralDenoiserWindow.h"

#include "AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPlotFloorDb = -80.0f;
constexpr float kPlotCeilingDb = 6.0f;

} // namespace

SpectralDenoiserProfileView::SpectralDenoiserProfileView(
    pluginrack::SpectralDenoiserEffect& effectToEdit)
    : effect(effectToEdit) {
    setInterceptsMouseClicks(true, false);
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

void SpectralDenoiserProfileView::ensureTrims() {
    if (effect.profileTrimDb.size()
        != static_cast<std::size_t>(pluginrack::kSpectralDenoiserTrimBandCount)) {
        effect.profileTrimDb.assign(pluginrack::kSpectralDenoiserTrimBandCount, 0.0f);
    }
}

juce::Rectangle<float> SpectralDenoiserProfileView::plotArea() const {
    return getLocalBounds().toFloat().reduced(8.0f, 20.0f);
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

    content.addAndMakeVisible(profileView);
    profileView.onTrimEdited = [this] {
        if (apply) apply(effect, false);
    };

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
    if (effect.hasProfile()) {
        statusLabel.setText("Noise profile saved" + trimText, juce::dontSendNotification);
    } else if (effect.mode == pluginrack::SpectralDenoiserMode::Learn) {
        statusLabel.setText("Learn a noise-only profile to reduce noise",
                            juce::dontSendNotification);
    } else {
        statusLabel.setText("Adaptive: tracks the noise floor as it plays" + trimText,
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

    area.removeFromTop(4);
    statusLabel.setBounds(area.removeFromTop(18));

    area.removeFromTop(6);
    const int knobHeight = juce::jlimit(110, 170, area.getHeight() - 130);
    const int graphHeight = juce::jmax(120, area.getHeight() - knobHeight - 8);
    profileView.setBounds(area.removeFromTop(graphHeight));
    area.removeFromTop(8);

    const auto knobArea = area.removeFromTop(juce::jmin(knobHeight, area.getHeight()));
    theme::ui::placeWindowKnob(knobArea.withSizeKeepingCentre(150, knobArea.getHeight()),
                               reductionLabel, reduction);
}
