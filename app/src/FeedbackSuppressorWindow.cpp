#include "FeedbackSuppressorWindow.h"

#include <algorithm>
#include <cmath>
#include <complex>
using namespace pluginrack;

namespace {
constexpr int kPeakHoldMs = 500;

float proportionOf(float linear) {
    if (linear <= 0.0f) return 0.0f;
    const float db = 20.0f * std::log10(linear);
    return juce::jlimit(0.0f, 1.0f, (db - analyzer::kMinDb) / analyzer::kDbRange);
}

juce::String dbText(float linear) {
    if (linear <= 0.0f) return "-Inf";
    const float db = 20.0f * std::log10(linear);
    if (db <= analyzer::kMinDb) return "-Inf";
    return juce::String(juce::jmin(db, analyzer::kMaxDb), 1);
}

int yForLevel(juce::Rectangle<int> bar, float proportion) {
    return juce::jlimit(bar.getY(), bar.getBottom(),
                        juce::roundToInt(static_cast<float>(bar.getBottom())
                                         - static_cast<float>(bar.getHeight()) * proportion));
}

float smoothedRms(float current, float target) {
    const float weight = target > current ? 0.5f : 0.12f;
    return current + (target - current) * weight;
}

double responseDbAt(double freq, const std::array<float, 8>& freqs,
                    const std::array<float, 8>& depthsDb, int count, double sampleRate) {
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    const double analysisBinWidth = sampleRate / pluginrack::kFeedbackSuppressorAnalysisSize;
    double db = 0.0;
    for (int i = 0; i < count; ++i) {
        const double notchHz = freqs[static_cast<std::size_t>(i)];
        const double attenuationDb = depthsDb[static_cast<std::size_t>(i)];
        const double a = std::pow(10.0, -attenuationDb / 40.0);
        const double w0 = 2.0 * juce::MathConstants<double>::pi * notchHz / sampleRate;
        const double q = static_cast<double>(pluginrack::feedbackSuppressorNotchQ(
            static_cast<float>(attenuationDb), static_cast<float>(notchHz),
            static_cast<float>(analysisBinWidth)));
        const double alpha = std::sin(w0) / (2.0 * q);
        const double cosw = std::cos(w0);
        const double a0 = 1.0 + alpha / a;
        const double b0 = (1.0 + alpha * a) / a0;
        const double b1 = (-2.0 * cosw) / a0;
        const double b2 = (1.0 - alpha * a) / a0;
        const double a1 = (-2.0 * cosw) / a0;
        const double a2 = (1.0 - alpha / a) / a0;
        const std::complex<double> num = b0 + b1 * z1 + b2 * z2;
        const std::complex<double> den = 1.0 + a1 * z1 + a2 * z2;
        db += 20.0 * std::log10(std::max(std::abs(num / den), 1.0e-9));
    }
    return db;
}
double biquadMagnitudeDb(double freq, double b0, double b1, double b2,
                         double a1, double a2, double sampleRate) {
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> num = b0 + b1 * z1 + b2 * z2;
    const std::complex<double> den = 1.0 + a1 * z1 + a2 * z2;
    return 20.0 * std::log10(std::max(std::abs(num / den), 1.0e-9));
}

double detectorFilterDbAt(double freq, float highPassHz, float lowPassHz,
                          double sampleRate) {
    static constexpr double kButterworthQ[4] = {
        0.5097955791041592, 0.6013448869350453,
        0.8999762231364156, 2.5629154477415055};
    const double fs = std::max(1.0, sampleRate);
    const double hp = std::clamp(static_cast<double>(highPassHz), 10.0,
                                 std::min(20000.0, fs * 0.45));
    const double lp = std::clamp(static_cast<double>(lowPassHz), 40.0,
                                 std::min(20000.0, fs * 0.475));
    double db = 0.0;
    for (const double q : kButterworthQ) {
        const double w0 = 2.0 * juce::MathConstants<double>::pi * hp / fs;
        const double cosw = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        db += biquadMagnitudeDb(freq, (1.0 + cosw) * 0.5 / a0,
                                -(1.0 + cosw) / a0, (1.0 + cosw) * 0.5 / a0,
                                -2.0 * cosw / a0, (1.0 - alpha) / a0, fs);
    }
    for (const double q : kButterworthQ) {
        const double w0 = 2.0 * juce::MathConstants<double>::pi * lp / fs;
        const double cosw = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        db += biquadMagnitudeDb(freq, (1.0 - cosw) * 0.5 / a0,
                                (1.0 - cosw) / a0, (1.0 - cosw) * 0.5 / a0,
                                -2.0 * cosw / a0, (1.0 - alpha) / a0, fs);
    }
    return db;
}
} // namespace

void FeedbackSuppressorGraph::setNotches(const float* freqs, const float* depthsDb, int count) {
    notchCount = juce::jlimit(0, static_cast<int>(notchFreqs.size()), count);
    for (int i = 0; i < notchCount; ++i) {
        const auto index = static_cast<std::size_t>(i);
        notchFreqs[index] = freqs[i];
        notchDepthsDb[index] = depthsDb != nullptr ? depthsDb[i] : 0.0f;
    }
    repaint();
}

void FeedbackSuppressorGraph::setSpectrum(const std::vector<float>& values) {
    spectrum = values;
    repaint();
}

void FeedbackSuppressorGraph::setSpectrumOffsetDb(float offsetDb) {
    if (spectrumOffsetDb == offsetDb) return;
    spectrumOffsetDb = offsetDb;
    repaint();
}

void FeedbackSuppressorGraph::setWeighting(SpectrumWeighting value) {
    if (weighting == value) return;
    weighting = value;
    repaint();
}

void FeedbackSuppressorGraph::setInputSensitivityDb(float db) {
    if (inputSensitivityDb == db) return;
    inputSensitivityDb = db;
    repaint();
}

void FeedbackSuppressorGraph::setDetectorBand(float highPassHz, float lowPassHz) {
    if (detectorHighPassHz == highPassHz && detectorLowPassHz == lowPassHz) return;
    detectorHighPassHz = highPassHz;
    detectorLowPassHz = lowPassHz;
    repaint();
}

void FeedbackSuppressorGraph::setSampleRate(double value) {
    sampleRateHz = value > 0.0 ? value : 48000.0;
    repaint();
}

void FeedbackSuppressorGraph::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
    auto bounds = getLocalBounds().reduced(1);
    if (bounds.getWidth() < 40 || bounds.getHeight() < 40) return;

    g.setColour(theme::panel());
    g.fillRoundedRectangle(bounds.toFloat(), 3.0f);

    auto plot = bounds.reduced(8);
    auto title = plot.removeFromTop(14);
    auto footer = plot.removeFromBottom(13);
    auto axis = plot.removeFromLeft(30);
    plot.removeFromLeft(2);

    const auto xForHz = [&plot](double hz) {
        return plot.getX() + juce::roundToInt(analyzer::frequencyToFraction(hz)
                                              * static_cast<double>(plot.getWidth()));
    };
    const auto yForDb = [&plot](double db) {
        return plot.getBottom() - juce::roundToInt(analyzer::dbToFraction(static_cast<float>(db))
                                                    * static_cast<double>(plot.getHeight()));
    };

    // dB grid. The analyzer and the notch response share this scale.
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    for (int db = -60; db <= 12; db += 12) {
        const int y = yForDb(static_cast<double>(db));
        g.setColour(theme::line().withAlpha(db == 0 ? 0.6f : 0.24f));
        g.drawHorizontalLine(y, (float)plot.getX(), (float)plot.getRight());
        if (db == -60 || db == -36 || db == -12 || db == 12) {
            g.setColour(theme::dim());
            g.drawText(juce::String(db), axis.getX(), y - 5, axis.getWidth() - 3, 10,
                       juce::Justification::centredRight, false);
        }
    }

    // Frequency grid and labels.
    for (const int hz : {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000}) {
        const int x = xForHz(static_cast<double>(hz));
        g.setColour(theme::line().withAlpha(0.22f));
        g.drawVerticalLine(x, (float)plot.getY(), (float)plot.getBottom());
        if (hz == 100 || hz == 1000 || hz == 10000) {
            g.setColour(theme::dim());
            g.drawText(hz >= 1000 ? juce::String(hz / 1000) + "k" : juce::String(hz),
                       x - 14, footer.getY(), 28, footer.getHeight(),
                       juce::Justification::centred, false);
        }
    }

    // Detector band: the RTA stays visible, but the shaded regions are where
    // feedback detection is disabled. The two amber guides make the HPF/LPF
    // knobs visible on the same frequency axis as the spectrum.
    const int highPassX = juce::jlimit(plot.getX(), plot.getRight(),
                                       xForHz(static_cast<double>(detectorHighPassHz)));
    const int lowPassX = juce::jlimit(plot.getX(), plot.getRight(),
                                      xForHz(static_cast<double>(detectorLowPassHz)));
    g.setColour(theme::bg().withAlpha(0.55f));
    g.fillRect(plot.getX(), plot.getY(), juce::jmax(0, highPassX - plot.getX()),
               plot.getHeight());
    g.fillRect(lowPassX, plot.getY(), juce::jmax(0, plot.getRight() - lowPassX),
               plot.getHeight());
    g.setColour(theme::amber().withAlpha(0.55f));
    g.drawVerticalLine(highPassX, (float)plot.getY(), (float)plot.getBottom());
    g.drawVerticalLine(lowPassX, (float)plot.getY(), (float)plot.getBottom());

    if (!spectrum.empty()) {
        TraceGraphComponent::drawSpectrum(g, plot.toFloat(), spectrum, sampleRateHz,
                                          spectrumOffsetDb, weighting);
    }

    // Combined notch response. A narrow notch is the whole point, so sample
    // every pixel instead of smoothing it away.
    juce::Path curve;
    for (int x = plot.getX(); x <= plot.getRight(); ++x) {
        const double t = static_cast<double>(x - plot.getX())
                         / juce::jmax(1, plot.getWidth());
        const double hz = analyzer::kMinFrequencyHz
                          * std::pow(analyzer::kMaxFrequencyHz / analyzer::kMinFrequencyHz, t);
        const double db = responseDbAt(hz, notchFreqs, notchDepthsDb, notchCount, sampleRateHz)
                          + detectorFilterDbAt(hz, detectorHighPassHz, detectorLowPassHz,
                                               sampleRateHz);
        const auto y = static_cast<float>(juce::jlimit(
            plot.getY(), plot.getBottom(), yForDb(db)));
        if (x == plot.getX()) curve.startNewSubPath((float)x, y);
        else curve.lineTo((float)x, y);
    }
    g.setColour(notchCount > 0 ? theme::cyan() : theme::dim().withAlpha(0.6f));
    g.strokePath(curve, juce::PathStrokeType(1.6f));

    if (notchCount > 0) {
        for (int i = 0; i < notchCount; ++i) {
            const int x = xForHz(notchFreqs[static_cast<std::size_t>(i)]);
            g.setColour(theme::amber().withAlpha(0.55f));
            g.drawVerticalLine(x, (float)plot.getY(), (float)plot.getBottom());
            g.fillEllipse((float)x - 2.0f, (float)plot.getY() - 2.0f, 4.0f, 4.0f);
        }
    }

    g.setColour(theme::dim());
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.drawText(notchCount > 0 ? juce::String(notchCount) + (notchCount == 1 ? " NARROW CUT" : " NARROW CUTS")
                              : "LISTENING - NO FEEDBACK DETECTED",
               title, juce::Justification::centredLeft, false);
    g.drawText("20 Hz", footer.withWidth(32), juce::Justification::centredLeft, false);
    g.drawText("20 kHz", footer.withTrimmedLeft(footer.getWidth() - 36),
               juce::Justification::centredRight, false);

    g.setColour(theme::line().withAlpha(0.6f));
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 3.0f, 1.0f);
}

FeedbackSuppressorMeter::FeedbackSuppressorMeter(juce::String captionText)
    : caption(std::move(captionText)) {}

void FeedbackSuppressorMeter::setLevels(float peakLinear, float rmsLinear) {
    peak = juce::jmax(peakLinear, peak * 0.85f);
    rms = smoothedRms(rms, juce::jmax(0.0f, rmsLinear));
    const auto now = juce::Time::getMillisecondCounter();
    if (peak >= held || now - heldAt >= static_cast<juce::uint32>(kPeakHoldMs)) {
        held = peak;
        heldAt = now;
    }
    repaint();
}

void FeedbackSuppressorMeter::paint(juce::Graphics& g) {
    g.fillAll(theme::bg().darker(0.2f));

    const auto well = getLocalBounds().reduced(1);
    if (well.isEmpty()) return;
    g.setColour(theme::panel());
    g.fillRoundedRectangle(well.toFloat(), 2.0f);

    auto content = well.reduced(1);
    if (content.getWidth() < 12 || content.getHeight() < 12) {
        g.setColour(theme::line().withAlpha(0.65f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 2.0f, 1.0f);
        return;
    }

    auto readout = content.removeFromTop(14);
    auto captionArea = content.removeFromBottom(12);
    auto bar = content.reduced(3, 0);
    if (bar.isEmpty()) return;

    g.setColour(theme::bg());
    g.fillRoundedRectangle(bar.toFloat(), 1.5f);

    const float rmsProportion = proportionOf(rms);
    const float peakProportion = proportionOf(peak);
    const float holdProportion = proportionOf(held);
    const int rmsTop = yForLevel(bar, rmsProportion);
    const int peakTop = yForLevel(bar, juce::jmax(peakProportion, rmsProportion));

    if (peakTop < rmsTop) {
        g.setColour(theme::line().withAlpha(0.75f));
        g.fillRect(bar.getX(), peakTop, bar.getWidth(), rmsTop - peakTop);
    }
    if (rmsTop < bar.getBottom()) {
        juce::ColourGradient grad(theme::green(), (float)bar.getX(), (float)bar.getBottom(),
                                 theme::amber(), (float)bar.getX(), (float)bar.getY(), false);
        g.setGradientFill(grad);
        g.fillRect(bar.getX(), rmsTop, bar.getWidth(), bar.getBottom() - rmsTop);
    }
    if (holdProportion > 0.0f) {
        const int holdY = juce::jlimit(bar.getY(), bar.getBottom() - 2,
                                       yForLevel(bar, holdProportion));
        g.setColour(theme::text());
        g.fillRect(bar.getX(), holdY, bar.getWidth(), 2);
    }

    g.setColour(theme::dim());
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    g.drawText(dbText(peak), readout, juce::Justification::centred, false);
    g.drawText(caption, captionArea, juce::Justification::centred, false);

    g.setColour(theme::line().withAlpha(0.65f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 2.0f, 1.0f);
}

FeedbackSuppressorWindow::FeedbackSuppressorWindow(const juce::String& title,
                                                   const FeedbackSuppressorEffect& initialEffect,
                                                   ApplyCallback applyCallback,
                                                   float initialRtaOffsetDb,
                                                   int initialWeighting,
                                                   double initialSampleRateHz)
    : juce::DocumentWindow(title, theme::strip(), juce::DocumentWindow::allButtons),
      effect(initialEffect), apply(std::move(applyCallback)),
      sampleRateHz(initialSampleRateHz > 0.0 ? initialSampleRateHz : 48000.0),
      analyzerFft(std::make_unique<juce::dsp::FFT>(analyzer::kFftOrder)) {
    setResizable(true, true);
    setResizeLimits(400, 400, 1600, 1000);
    setContentOwned(&content, false);
    setUsingNativeTitleBar(true);
    centreWithSize(kWidth, kHeight);

    hannCoeffs.resize(static_cast<std::size_t>(kAnalyzerFftSize));
    for (int i = 0; i < kAnalyzerFftSize; ++i) {
        const double t = juce::MathConstants<double>::twoPi * i / kAnalyzerFftSize;
        hannCoeffs[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(t));
    }
    fftScratch.assign(static_cast<std::size_t>(kAnalyzerFftSize) * 2, 0.0f);
    tapL.assign(static_cast<std::size_t>(kAnalyzerFftSize), 0.0f);
    tapR.assign(static_cast<std::size_t>(kAnalyzerFftSize), 0.0f);
    outL.assign(static_cast<std::size_t>(kAnalyzerFftSize), 0.0f);
    outR.assign(static_cast<std::size_t>(kAnalyzerFftSize), 0.0f);
    spectrumDb.assign(static_cast<std::size_t>(kAnalyzerFftSize) / 2 + 1, 0.0f);
    spectrumAvg.assign(static_cast<std::size_t>(kAnalyzerFftSize) / 2 + 1, -100.0f);

    theme::ui::styleTitleLabel(modeLabel);
    content.addAndMakeVisible(modeLabel);

    content.addAndMakeVisible(analyzerLabel);
    content.addAndMakeVisible(rtaMinusButton);
    content.addAndMakeVisible(rtaOffsetLabel);
    content.addAndMakeVisible(rtaPlusButton);
    content.addAndMakeVisible(weightingZ);
    content.addAndMakeVisible(weightingA);
    content.addAndMakeVisible(weightingC);

    analyzerLabel.setJustificationType(juce::Justification::centredRight);
    analyzerLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    analyzerLabel.setColour(juce::Label::textColourId, theme::dim());
    rtaMinusButton.setTooltip("RTA display: shift the analyzer down");
    rtaMinusButton.setFontSize(9.0f);
    rtaMinusButton.setClickAction([this] { setRtaOffsetDb(rtaOffsetDb - kTraceRtaStepDb); });
    rtaPlusButton.setTooltip("RTA display: shift the analyzer up");
    rtaPlusButton.setFontSize(9.0f);
    rtaPlusButton.setClickAction([this] { setRtaOffsetDb(rtaOffsetDb + kTraceRtaStepDb); });
    rtaOffsetLabel.setJustificationType(juce::Justification::centred);
    rtaOffsetLabel.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    rtaOffsetLabel.setColour(juce::Label::textColourId, theme::dim());
    rtaOffsetLabel.setTooltip("RTA display offset in dB, saved with the scene");
    const char* weightingTips[3] = {
        "Z: flat - the spectrum as measured", "A: A-weighting (IEC 61672)", "C: C-weighting (IEC 61672)"};
    for (int i = 0; i < 3; ++i) {
        weightingButtons[static_cast<std::size_t>(i)]->setTooltip(weightingTips[i]);
        weightingButtons[static_cast<std::size_t>(i)]->setFontSize(9.0f);
        weightingButtons[static_cast<std::size_t>(i)]->setClickAction(
            [this, i] { setWeightingIndex(i); });
    }
    setWeightingIndex(initialWeighting);
    setRtaOffsetDb(initialRtaOffsetDb);

    enableButton.setClickingTogglesState(true);
    enableButton.setToggleState(!effect.bypassed, juce::dontSendNotification);
    theme::ui::styleButton(enableButton);
    enableButton.onClick = [this] {
        effect.bypassed = !enableButton.getToggleState();
        if (apply) apply(effect, true);
    };
    content.addAndMakeVisible(enableButton);

    styleKnob(strength, kFeedbackSuppressorMinCuts, kFeedbackSuppressorMaxCuts, 1.0,
              "", kFeedbackSuppressorDefaultCuts, theme::cyan());
    strength.textFromValueFunction = [](double value) {
        const int cuts = juce::jlimit(kFeedbackSuppressorMinCuts,
                                      kFeedbackSuppressorMaxCuts,
                                      juce::roundToInt(value));
        return juce::String(cuts) + (cuts == 1 ? " cut" : " cuts");
    };
    strength.valueFromTextFunction = [](const juce::String& text) {
        return static_cast<double>(juce::jlimit(kFeedbackSuppressorMinCuts,
                                                kFeedbackSuppressorMaxCuts,
                                                text.getIntValue()));
    };
    strength.setTooltip("Maximum number of simultaneous narrow cuts");
    strength.onValueChange = [this] {
        const int cuts = juce::jlimit(kFeedbackSuppressorMinCuts,
                                      kFeedbackSuppressorMaxCuts,
                                      juce::roundToInt(strength.getValue()));
        effect.strength = feedbackSuppressorStrengthForCuts(cuts);
        graph.setNotches(lastNotches.data(), lastDepths.data(), lastNotchCount);
        if (apply) apply(effect, false);
    };
    content.addAndMakeVisible(strength);
    content.addAndMakeVisible(strengthLabel);

    styleKnob(attenuation, kFeedbackSuppressorMinAttenuationDb,
              kFeedbackSuppressorMaxAttenuationDb, 1.0, " dB",
              kFeedbackSuppressorDefaultAttenuationDb, theme::amber());
    attenuation.setTooltip("Depth of each cut. Deeper cuts widen automatically");
    attenuation.onValueChange = [this] {
        effect.attenuationDb = static_cast<float>(attenuation.getValue());
        graph.setNotches(lastNotches.data(), lastDepths.data(), lastNotchCount);
        if (apply) apply(effect, false);
    };
    content.addAndMakeVisible(attenuation);
    content.addAndMakeVisible(attenuationLabel);

    styleKnob(gainReductionMultiplier,
              kFeedbackSuppressorMinGainReductionMultiplier,
              kFeedbackSuppressorMaxGainReductionMultiplier, 0.05, "x",
              kFeedbackSuppressorDefaultGainReductionMultiplier, theme::amber());
    gainReductionMultiplier.textFromValueFunction = [](double value) {
        return juce::String(value, 2) + "x";
    };
    gainReductionMultiplier.setTooltip(
        "Multiplies the detected gain reduction before the Attenuation cap");
    gainReductionMultiplier.onValueChange = [this] {
        effect.gainReductionMultiplier = static_cast<float>(gainReductionMultiplier.getValue());
        if (apply) apply(effect, false);
    };
    content.addAndMakeVisible(gainReductionMultiplier);
    content.addAndMakeVisible(gainReductionMultiplierLabel);

    styleKnob(sensitivity, kFeedbackSuppressorMinSensitivityDb,
              kFeedbackSuppressorMaxSensitivityDb, 1.0, " dB",
              kFeedbackSuppressorDefaultSensitivityDb, theme::amber());
    sensitivity.onValueChange = [this] {
        effect.inputSensitivityDb = static_cast<float>(sensitivity.getValue());
        graph.setInputSensitivityDb(effect.inputSensitivityDb);
        if (apply) apply(effect, false);
    };
    content.addAndMakeVisible(sensitivity);
    content.addAndMakeVisible(sensitivityLabel);

    styleKnob(highPass, kFeedbackSuppressorMinDetectorHz,
              kFeedbackSuppressorMaxHighPassHz,
              1.0, " Hz", kFeedbackSuppressorDefaultHighPassHz, theme::cyan());
    highPass.setSkewFactorFromMidPoint(80.0);
    highPass.setTooltip("Detector high-pass: 48 dB/oct below this frequency");
    highPass.onValueChange = [this] {
        const float hp = static_cast<float>(highPass.getValue());
        effect.detectorHighPassHz = juce::jlimit(
            kFeedbackSuppressorMinDetectorHz,
            kFeedbackSuppressorMaxHighPassHz, hp);
        highPass.setValue(effect.detectorHighPassHz, juce::dontSendNotification);
        graph.setDetectorBand(effect.detectorHighPassHz, effect.detectorLowPassHz);
        if (apply) apply(effect, false);
    };
    content.addAndMakeVisible(highPass);
    content.addAndMakeVisible(highPassLabel);

    styleKnob(lowPass, kFeedbackSuppressorMinLowPassHz,
              kFeedbackSuppressorMaxDetectorHz, 1.0, " Hz",
              kFeedbackSuppressorDefaultLowPassHz, theme::cyan());
    lowPass.setSkewFactorFromMidPoint(8000.0);
    lowPass.setTooltip("Detector low-pass: 48 dB/oct above this frequency");
    lowPass.onValueChange = [this] {
        const float lp = static_cast<float>(lowPass.getValue());
        effect.detectorLowPassHz = juce::jlimit(
            kFeedbackSuppressorMinLowPassHz,
            kFeedbackSuppressorMaxDetectorHz, lp);
        lowPass.setValue(effect.detectorLowPassHz, juce::dontSendNotification);
        graph.setDetectorBand(effect.detectorHighPassHz, effect.detectorLowPassHz);
        if (apply) apply(effect, false);
    };
    content.addAndMakeVisible(lowPass);
    content.addAndMakeVisible(lowPassLabel);

    styleKnob(attack, kFeedbackSuppressorMinAttackMs, kFeedbackSuppressorMaxAttackMs,
              1.0, " ms", kFeedbackSuppressorDefaultAttackMs, theme::cyan());
    attack.setTooltip("How quickly a newly detected cut fades in");
    attack.onValueChange = [this] {
        effect.attackMs = static_cast<float>(attack.getValue());
        if (apply) apply(effect, false);
    };
    content.addAndMakeVisible(attack);
    content.addAndMakeVisible(attackLabel);

    styleKnob(hold, kFeedbackSuppressorMinHoldMs, kFeedbackSuppressorMaxHoldMs,
              10.0, " ms", kFeedbackSuppressorDefaultHoldMs, theme::amber());
    hold.setTooltip("How long a cut stays after the ringing stops");
    hold.onValueChange = [this] {
        effect.holdMs = static_cast<float>(hold.getValue());
        if (apply) apply(effect, false);
    };
    content.addAndMakeVisible(hold);
    content.addAndMakeVisible(holdLabel);

    styleKnob(release, kFeedbackSuppressorMinReleaseMs, kFeedbackSuppressorMaxReleaseMs,
              10.0, " ms", kFeedbackSuppressorDefaultReleaseMs, theme::amber());
    release.setTooltip("How quickly a cut fades out after Hold");
    release.onValueChange = [this] {
        effect.releaseMs = static_cast<float>(release.getValue());
        if (apply) apply(effect, false);
    };
    content.addAndMakeVisible(release);
    content.addAndMakeVisible(releaseLabel);

    styleKnob(detectionDelay, kFeedbackSuppressorMinDetectionDelayMs,
              kFeedbackSuppressorMaxDetectionDelayMs, 10.0, " ms",
              kFeedbackSuppressorDefaultDetectionDelayMs, theme::cyan());
    detectionDelay.setTooltip(
        "How long a new tone must persist before it becomes a cut; "
        "longer values reject more voice peaks");
    detectionDelay.onValueChange = [this] {
        effect.detectionDelayMs = static_cast<float>(detectionDelay.getValue());
        if (apply) apply(effect, false);
    };
    content.addAndMakeVisible(detectionDelay);
    content.addAndMakeVisible(detectionDelayLabel);

    content.addAndMakeVisible(graph);
    content.addAndMakeVisible(inputMeter);
    content.addAndMakeVisible(outputMeter);
    graph.setSampleRate(sampleRateHz);
    graph.setSpectrumOffsetDb(spectrumOffsetDb);
    graph.setWeighting(weighting);
    graph.setInputSensitivityDb(effect.inputSensitivityDb);
    graph.setDetectorBand(effect.detectorHighPassHz, effect.detectorLowPassHz);

    syncControls();
    setVisible(true);
    startTimer(33);
}

void FeedbackSuppressorWindow::styleKnob(ValueEditSlider& slider, double min, double max,
                                         double step, const juce::String& suffix,
                                         double resetValue, juce::Colour fill) {
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 18);
    slider.setRange(min, max, step);
    slider.setTextValueSuffix(suffix);
    slider.setDoubleClickReturnValue(true, resetValue);
    slider.setColour(juce::Slider::rotarySliderFillColourId, fill);
    slider.setColour(juce::Slider::rotarySliderOutlineColourId, theme::border());
    slider.setColour(juce::Slider::textBoxTextColourId, theme::text());
    slider.setColour(juce::Slider::textBoxBackgroundColourId, theme::control());
    slider.setColour(juce::Slider::textBoxOutlineColourId, theme::line());
}

void FeedbackSuppressorWindow::syncControls() {
    const int cuts = juce::jlimit(kFeedbackSuppressorMinCuts, kFeedbackSuppressorMaxCuts,
                                  juce::roundToInt(effect.strength * 8.0f));
    strength.setValue(static_cast<double>(cuts), juce::dontSendNotification);
    attenuation.setValue(effect.attenuationDb, juce::dontSendNotification);
    gainReductionMultiplier.setValue(effect.gainReductionMultiplier,
                                     juce::dontSendNotification);
    sensitivity.setValue(effect.inputSensitivityDb, juce::dontSendNotification);
    highPass.setValue(effect.detectorHighPassHz, juce::dontSendNotification);
    lowPass.setValue(effect.detectorLowPassHz, juce::dontSendNotification);
    attack.setValue(effect.attackMs, juce::dontSendNotification);
    hold.setValue(effect.holdMs, juce::dontSendNotification);
    release.setValue(effect.releaseMs, juce::dontSendNotification);
    detectionDelay.setValue(effect.detectionDelayMs, juce::dontSendNotification);
    const auto styleCaption = [](juce::Label& label) {
        label.setJustificationType(juce::Justification::centred);
        label.setFont(juce::Font(juce::FontOptions(11.0f)));
        label.setColour(juce::Label::textColourId, theme::dim());
    };
    styleCaption(strengthLabel);
    styleCaption(attenuationLabel);
    styleCaption(gainReductionMultiplierLabel);
    styleCaption(sensitivityLabel);
    styleCaption(highPassLabel);
    styleCaption(lowPassLabel);
    styleCaption(attackLabel);
    styleCaption(holdLabel);
    styleCaption(releaseLabel);
    styleCaption(detectionDelayLabel);
    graph.setDetectorBand(effect.detectorHighPassHz, effect.detectorLowPassHz);
    graph.setNotches(lastNotches.data(), lastDepths.data(), lastNotchCount);
}

void FeedbackSuppressorWindow::setBypassed(bool b) {
    effect.bypassed = b;
    enableButton.setToggleState(!b, juce::dontSendNotification);
}

void FeedbackSuppressorWindow::setRtaOffsetDb(float offsetDb) {
    const float clamped = juce::jlimit(kTraceRtaMinOffsetDb, kTraceRtaMaxOffsetDb, offsetDb);
    rtaOffsetDb = clamped;
    spectrumOffsetDb = kTraceRtaDefaultOffsetDb + clamped;
    const int shown = juce::roundToInt(clamped);
    rtaOffsetLabel.setText((shown > 0 ? "+" : "") + juce::String(shown), juce::dontSendNotification);
    graph.setSpectrumOffsetDb(spectrumOffsetDb);
    if (applySpectrumOffset) applySpectrumOffset(clamped);
}

void FeedbackSuppressorWindow::setWeightingIndex(int index) {
    const int clamped = juce::jlimit(0, 2, index);
    const bool changed = clamped != weightingIndex;
    weightingIndex = clamped;
    weighting = weightingFromIndex(clamped);
    for (int i = 0; i < 3; ++i)
        weightingButtons[static_cast<std::size_t>(i)]->setToggleState(i == clamped);
    graph.setWeighting(weighting);
    if (!changed) return;
    if (applyWeighting) applyWeighting(clamped);
}

void FeedbackSuppressorWindow::timerCallback() {
    if (pullNotches) {
        lastNotchCount = pullNotches(lastNotches.data(), lastDepths.data(),
                                     static_cast<int>(lastNotches.size()));
        graph.setNotches(lastNotches.data(), lastDepths.data(), lastNotchCount);
    }
    if (!pullAnalyzerTap) return;
    pullAnalyzerTap(false, tapL.data(), tapR.data(), kAnalyzerFftSize);
    pullAnalyzerTap(true, outL.data(), outR.data(), kAnalyzerFftSize);

    const auto measure = [](const auto& l, const auto& r, float& peak, float& rms) {
        float p = 0.0f;
        double sum = 0.0;
        for (std::size_t i = 0; i < l.size(); ++i) {
            p = std::max(p, std::max(std::fabs(l[i]), std::fabs(r[i])));
            sum += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
        }
        peak = p;
        rms = static_cast<float>(std::sqrt(sum / (2.0 * static_cast<double>(l.size()))));
    };

    float inPeak = 0.0f, inRms = 0.0f, outPeak = 0.0f, outRms = 0.0f;
    measure(tapL, tapR, inPeak, inRms);
    measure(outL, outR, outPeak, outRms);
    inputMeter.setLevels(inPeak, inRms);
    outputMeter.setLevels(outPeak, outRms);

    std::fill(fftScratch.begin(), fftScratch.end(), 0.0f);
    for (int i = 0; i < kAnalyzerFftSize; ++i) {
        const std::size_t idx = static_cast<std::size_t>(i);
        fftScratch[idx] = (outL[idx] + outR[idx]) * 0.5f * hannCoeffs[idx];
    }
    analyzerFft->performFrequencyOnlyForwardTransform(fftScratch.data(), true);
    constexpr float normalisation = 4.0f / static_cast<float>(kAnalyzerFftSize);
    for (std::size_t i = 0; i < spectrumDb.size(); ++i) {
        const float magnitude = fftScratch[i] * normalisation;
        spectrumDb[i] = juce::jmax(-100.0f, 20.0f * std::log10(juce::jmax(1.0e-5f, magnitude)));
    }
    for (std::size_t bin = 0; bin < spectrumDb.size(); ++bin) {
        const float fresh = spectrumDb[bin];
        float& held = spectrumAvg[bin];
        held += (fresh - held) * (fresh > held ? 0.6f : 0.15f);
    }
    graph.setSpectrum(spectrumAvg);
    graph.repaint();
}

void FeedbackSuppressorWindow::resized() {
    content.setBounds(getLocalBounds());
    auto area = content.getLocalBounds().reduced(10);
    modeLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);

    auto analyzerRow = area.removeFromTop(24);
    analyzerLabel.setBounds(analyzerRow.removeFromLeft(62));
    rtaMinusButton.setBounds(analyzerRow.removeFromLeft(24).reduced(1));
    rtaOffsetLabel.setBounds(analyzerRow.removeFromLeft(34));
    rtaPlusButton.setBounds(analyzerRow.removeFromLeft(24).reduced(1));
    analyzerRow.removeFromLeft(8);
    for (int i = 0; i < 3; ++i) {
        weightingButtons[static_cast<std::size_t>(i)]->setBounds(
            analyzerRow.removeFromLeft(24).reduced(1));
        analyzerRow.removeFromLeft(2);
    }
    area.removeFromTop(6);

    // Wide (docked / full-size) layout: a controls rail on the left and the
    // RTA taking all remaining space. The compact floating window stacks the
    // RTA above a single control row instead.
    if (area.getWidth() >= 620) {
        auto rail = area.removeFromLeft(230);
        graph.setBounds(area);
        enableButton.setBounds(rail.removeFromTop(24).removeFromLeft(100));
        rail.removeFromTop(6);

        auto topKnobs = rail.removeFromTop(84);
        theme::ui::placeWindowKnob(topKnobs.removeFromLeft(76), strengthLabel, strength);
        theme::ui::placeWindowKnob(topKnobs.removeFromLeft(76), attenuationLabel, attenuation);
        theme::ui::placeWindowKnob(topKnobs, sensitivityLabel, sensitivity);

        rail.removeFromTop(4);
        auto bandKnobs = rail.removeFromTop(84);
        theme::ui::placeWindowKnob(bandKnobs.removeFromLeft(76), highPassLabel, highPass);
        theme::ui::placeWindowKnob(bandKnobs.removeFromLeft(76), lowPassLabel, lowPass);
        theme::ui::placeWindowKnob(bandKnobs, gainReductionMultiplierLabel,
                                   gainReductionMultiplier);

        rail.removeFromTop(4);
        auto envelopeKnobs = rail.removeFromTop(84);
        theme::ui::placeWindowKnob(envelopeKnobs.removeFromLeft(76), attackLabel, attack);
        theme::ui::placeWindowKnob(envelopeKnobs.removeFromLeft(76), holdLabel, hold);
        theme::ui::placeWindowKnob(envelopeKnobs, releaseLabel, release);

        rail.removeFromTop(4);
        auto confirmKnobs = rail.removeFromTop(84);
        theme::ui::placeWindowKnob(confirmKnobs.removeFromLeft(76),
                                   detectionDelayLabel, detectionDelay);

        rail.removeFromTop(6);
        auto meters = rail.removeFromTop(juce::jmin(90, rail.getHeight()));
        inputMeter.setBounds(meters.removeFromLeft(62));
        meters.removeFromLeft(16);
        outputMeter.setBounds(meters.removeFromLeft(62));
    } else {
        const int graphHeight = juce::jlimit(80, 150, area.getHeight() * 35 / 100);
        graph.setBounds(area.removeFromTop(graphHeight));
        area.removeFromTop(4);

        auto left = area.removeFromLeft(juce::jmin(220, area.getWidth() * 55 / 100));
        const int rowHeight = juce::jlimit(48, 82, (left.getHeight() - 12) / 4);
        auto topKnobs = left.removeFromTop(rowHeight);
        theme::ui::placeWindowKnob(topKnobs.removeFromLeft(topKnobs.getWidth() / 3),
                                   strengthLabel, strength);
        theme::ui::placeWindowKnob(topKnobs.removeFromLeft(topKnobs.getWidth() / 2),
                                   attenuationLabel, attenuation);
        theme::ui::placeWindowKnob(topKnobs, sensitivityLabel, sensitivity);
        left.removeFromTop(4);
        auto bandKnobs = left.removeFromTop(rowHeight);
        theme::ui::placeWindowKnob(bandKnobs.removeFromLeft(bandKnobs.getWidth() / 3),
                                   highPassLabel, highPass);
        theme::ui::placeWindowKnob(bandKnobs.removeFromLeft(bandKnobs.getWidth() / 2),
                                   lowPassLabel, lowPass);
        theme::ui::placeWindowKnob(bandKnobs, gainReductionMultiplierLabel,
                                   gainReductionMultiplier);
        left.removeFromTop(4);
        auto envelopeKnobs = left.removeFromTop(rowHeight);
        theme::ui::placeWindowKnob(envelopeKnobs.removeFromLeft(envelopeKnobs.getWidth() / 3),
                                   attackLabel, attack);
        theme::ui::placeWindowKnob(envelopeKnobs.removeFromLeft(envelopeKnobs.getWidth() / 2),
                                   holdLabel, hold);
        theme::ui::placeWindowKnob(envelopeKnobs, releaseLabel, release);
        left.removeFromTop(4);
        auto confirmKnobs = left.removeFromTop(rowHeight);
        theme::ui::placeWindowKnob(confirmKnobs.removeFromLeft(confirmKnobs.getWidth() / 2),
                                   detectionDelayLabel, detectionDelay);

        auto controls = area.reduced(4, 0);
        enableButton.setBounds(controls.removeFromTop(22).removeFromLeft(86));
        controls.removeFromTop(6);
        auto meters = controls.removeFromTop(juce::jmin(92, controls.getHeight()));
        inputMeter.setBounds(meters.removeFromLeft(52));
        meters.removeFromLeft(10);
        outputMeter.setBounds(meters.removeFromLeft(52));
    }
}
