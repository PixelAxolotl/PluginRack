#pragma once

#include <JuceHeader.h>

#include "AudioEngine.h"
#include "DeviceBackend.h"
#include "Theme.h"

// The per-stage latency budget. The single round-trip figure answers "how
// slow", never "where": this splits it into the terms the app chooses (read
// delay, clock FIFO) and the ones the driver reports, then shows what the
// model cannot explain. Drawn as one component so the rows stay aligned and
// the Settings layout only has to reserve a block of height.
class LatencyBudgetView final : public juce::Component {
public:
    void setReport(const AudioEngine::LatencyReport& report) {
        report_ = report;
        repaint();
    }

    static int preferredHeight() { return kRowH * kRows; }

    void paint(juce::Graphics& g) override {
        if (!report_.valid) return;

        const double blockPeriodMs = report_.sampleRate > 0.0 && report_.outputBufferSize > 0
                                         ? report_.outputBufferSize / report_.sampleRate * 1000.0
                                         : 0.0;
        // Anything beyond one device block of prediction error means a stage is
        // hiding buffering the report cannot see, so it earns a warning colour
        // rather than being silently absorbed into the total.
        const bool unexpectedIsLarge =
            report_.measuredValid && blockPeriodMs > 0.0
            && std::abs(report_.unexpectedMs) > blockPeriodMs;

        int row = 0;
        const auto drawRow = [&](const juce::String& label, const juce::String& detail,
                                 const juce::String& value, juce::Colour valueColour,
                                 bool bold) {
            auto bounds = juce::Rectangle<int>(0, row * kRowH, getWidth(), kRowH);
            g.setColour(bold ? theme::text() : theme::dim());
            g.setFont(bold ? theme::ui::bodyBoldFont() : theme::ui::captionFont());
            g.drawText(label, bounds, juce::Justification::centredLeft, true);
            if (detail.isNotEmpty()) {
                const int labelWidth = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), label);
                g.setColour(theme::dim().withAlpha(0.7f));
                g.drawText(detail, bounds.withTrimmedLeft(labelWidth + 8),
                           juce::Justification::centredLeft, true);
            }
            g.setColour(valueColour);
            g.setFont(bold ? theme::ui::bodyBoldFont() : theme::ui::bodyFont());
            g.drawText(value, bounds, juce::Justification::centredRight, false);
            ++row;
        };

        const auto ms = [](double value) {
            return juce::String(value, value < 10.0 ? 2 : 1) + " ms";
        };
        const auto frames = [](int value) { return juce::String(value) + " smp"; };

        // Whose number is the buffer? "Auto" means the driver picked it, so a
        // large size is a driver fact, not something the app chose.
        const juce::String bufferOrigin =
            report_.requestedOutputBufferSize > 0
                ? "asked " + frames(report_.requestedOutputBufferSize)
                : juce::String("driver default");
        const juce::String backend = deviceBackendTag(report_.clockDeviceType);
        const juce::String clockName = report_.clockDeviceType.isNotEmpty()
                                           ? backend
                                           : juce::String("Clock");
        drawRow(clockName,
                "buffer " + bufferOrigin + "  -  " + juce::String(juce::roundToInt(report_.sampleRate))
                    + " Hz",
                juce::String(),
                deviceBackendIsAsio(report_.clockDeviceType) ? theme::green() : theme::text(), false);

        drawRow("Input device", "driver", ms(report_.inputDeviceLatencyMs), theme::text(), false);
        drawRow("Input buffer", frames(report_.inputReadDelayFrames) + " read delay",
                ms(report_.inputFifoDelayMs), theme::text(), false);        drawRow(report_.maxInsertName.isNotEmpty() && report_.maxInsertMs > 0.0
                    ? "Plugin chain"
                    : "Plugins",
                report_.maxInsertMs > 0.0
                    ? "slowest: " + report_.maxInsertName + " " + ms(report_.maxInsertMs)
                    : juce::String(),
                ms(report_.pluginMs), report_.pluginMs > 20.0 ? theme::amber() : theme::text(), false);
        drawRow("Clock FIFO", report_.clockTargetFillFrames > 0
                                  ? frames(report_.clockFillFrames) + " of "
                                        + frames(report_.clockTargetFillFrames)
                                  : juce::String(),
                ms(report_.fifoMs), theme::text(), false);
        drawRow("Output device", "driver", ms(report_.outputDeviceLatencyMs), theme::text(), false);
        drawRow("Output buffer",
                report_.requestedOutputBufferSize > 0 ? "requested" : "driver default",
                frames(report_.outputBufferSize) + " / " + ms(report_.outputBufferMs),
                theme::text(), false);

        auto divider = juce::Rectangle<int>(0, row * kRowH + 2, getWidth(), 1);
        g.setColour(theme::border().withAlpha(0.5f));
        g.fillRect(divider);
        ++row;

        drawRow("Predicted", juce::String(), ms(report_.totalMs), theme::text(), true);
        drawRow("Measured", report_.measuredValid ? juce::String()
                                                  : (report_.measureFailed ? "no return" : "not run"),
                report_.measuredValid ? ms(report_.measuredMs) : juce::String("-"),
                report_.measuredValid ? theme::green() : theme::dim(), true);
        drawRow("Unexplained", "measured minus predicted",
                report_.measuredValid ? ms(report_.unexpectedMs) : juce::String("-"),
                unexpectedIsLarge ? theme::amber() : theme::dim(), false);

        // The read delay exists because capture and playback are bridged. Say
        // so where the number is, and say what removes it.
        if (report_.separateClocks && report_.inputReadDelayFrames > 0) {
            auto hint = juce::Rectangle<int>(0, row * kRowH, getWidth(), kRowH * 2);
            g.setColour(theme::amber().withAlpha(0.85f));
            g.setFont(theme::ui::captionFont());
            g.drawFittedText("Capture and playback are different devices, so the read delay "
                             "bridges two clocks. Put the clock on the input's interface "
                             "(Force ASIO) to remove it.",
                             hint, juce::Justification::topLeft, 2, 0.8f);
        }
    }

private:
    static constexpr int kRowH = 15;
    static constexpr int kRows = 12;
    AudioEngine::LatencyReport report_{};
};
