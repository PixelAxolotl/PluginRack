#include "SettingsView.h"

SettingsView::SettingsView(AudioEngine& engine)
    : audioEngine(engine) {
    titleLabel.setText("Settings", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(titleLabel);

    const auto styleLabel = [this](juce::Label& l) {
        l.setFont(juce::Font(juce::FontOptions(12.0f)));
        l.setColour(juce::Label::textColourId, theme::dim());
        addAndMakeVisible(l);
    };
    styleLabel(clockLabel);
    styleLabel(latencyLabel);
    styleLabel(bufferLabel);
    styleLabel(errorLabel);

    for (auto* label : { &inputBufferSizeLabel, &outputBufferSizeLabel }) {
        label->setFont(juce::Font(juce::FontOptions(12.0f)));
        label->setColour(juce::Label::textColourId, theme::text());
        addAndMakeVisible(*label);
    }
    inputBufferSizeLabel.setText("Input buffer", juce::dontSendNotification);
    outputBufferSizeLabel.setText("Output buffer", juce::dontSendNotification);

    for (auto* combo : { &inputBufferSizeCombo, &outputBufferSizeCombo }) {
        combo->addItem("Auto (driver default)", 0);
        for (int s : {32, 64, 128, 256, 512, 1024, 2048, 4096}) {
            combo->addItem(juce::String(s) + " samples", s);
        }
        combo->setColour(juce::ComboBox::backgroundColourId, theme::control());
        combo->setColour(juce::ComboBox::textColourId, theme::text());
        combo->setColour(juce::ComboBox::arrowColourId, theme::dim());
        combo->setColour(juce::ComboBox::outlineColourId, theme::border());
    }
    inputBufferSizeCombo.onChange = [this] {
        const auto err = audioEngine.setInputBufferSize(inputBufferSizeCombo.getSelectedId());
        refresh();
        if (err.isNotEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
        }
    };
    outputBufferSizeCombo.onChange = [this] {
        const auto err = audioEngine.setOutputBufferSize(outputBufferSizeCombo.getSelectedId());
        refresh();
        if (err.isNotEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
        }
    };
    addAndMakeVisible(inputBufferSizeCombo);
    addAndMakeVisible(outputBufferSizeCombo);

    measureButton.onClick = [this] {
        if (audioEngine.isLatencyMeasuring()) {
            audioEngine.cancelLatencyMeasurement();
        } else {
            audioEngine.startLatencyMeasurement();
        }
        refresh();
    };
    measureButton.setColour(juce::TextButton::buttonColourId, theme::control());
    measureButton.setColour(juce::TextButton::textColourOffId, theme::text());
    addAndMakeVisible(measureButton);

    measuredLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    measuredLabel.setColour(juce::Label::textColourId, theme::dim());
    addAndMakeVisible(measuredLabel);

    startTimer(250);
    refresh();
}

void SettingsView::refresh() {
    clockLabel.setText("Clock: " + audioEngine.getClockDescription(), juce::dontSendNotification);
    const auto report = audioEngine.getLatencyReport();
    if (report.valid) {
        latencyLabel.setText("Latency (est.): " + juce::String((int)std::round(report.totalMs)) + " ms", juce::dontSendNotification);
        bufferLabel.setText("Sample rate: " + juce::String((int)report.sampleRate) + " Hz   Buffer: "
                            + juce::String(report.inputBufferSize) + " in / " + juce::String(report.outputBufferSize) + " out",
                            juce::dontSendNotification);
        if (!inputBufferSizeCombo.isPopupActive()) {
            inputBufferSizeCombo.setSelectedId(report.inputBufferSize, juce::dontSendNotification);
        }
        if (!outputBufferSizeCombo.isPopupActive()) {
            outputBufferSizeCombo.setSelectedId(report.outputBufferSize, juce::dontSendNotification);
        }
    } else {
        latencyLabel.setText("Latency (est.): n/a", juce::dontSendNotification);
        bufferLabel.setText("Sample rate: n/a", juce::dontSendNotification);
    }

    if (audioEngine.isLatencyMeasuring()) {
        measuredLabel.setText("Measuring round-trip... (connect output to input)", juce::dontSendNotification);
        measureButton.setButtonText("Cancel Measurement");
    } else if (report.measuredValid) {
        measuredLabel.setText("Measured round-trip: " + juce::String((int)std::round(report.measuredMs)) + " ms", juce::dontSendNotification);
        measureButton.setButtonText("Measure Round-Trip Latency");
    } else if (report.measureFailed) {
        measuredLabel.setText("Measurement failed - no loopback detected", juce::dontSendNotification);
        measureButton.setButtonText("Measure Round-Trip Latency");
    } else {
        measuredLabel.setText("Measured round-trip: not measured", juce::dontSendNotification);
        measureButton.setButtonText("Measure Round-Trip Latency");
    }
    juce::String errors;
    for (const auto& c : audioEngine.getChannels()) {
        if (c->inputError.isNotEmpty()) errors += c->name + ": " + c->inputError + "\n";
    }
    for (const auto& o : audioEngine.getOutputs()) {
        if (o->outputError.isNotEmpty()) errors += o->name + ": " + o->outputError + "\n";
    }
    if (report.underruns + report.overruns > 0) errors += "Glitches: " + juce::String((juce::int64)report.overruns) + " overruns / " + juce::String((juce::int64)report.underruns) + " underruns\n";
    const auto fifo = audioEngine.getFifoDiagnostics();
    if (fifo.outputLowWatermarks > 0 || fifo.renderMisses > 0) {
        errors += "Safety: " + juce::String((juce::int64)fifo.outputLowWatermarks)
            + " low-watermarks / " + juce::String((juce::int64)fifo.renderMisses)
            + " render misses\n";
    }
    if (errors.isEmpty()) errors = "No audio errors.";
    errorLabel.setText(errors.trim(), juce::dontSendNotification);
}

void SettingsView::timerCallback() { refresh(); }

void SettingsView::resized() {
    auto area = getLocalBounds().reduced(24);
    titleLabel.setBounds(area.removeFromTop(30));
    area.removeFromTop(8);
    clockLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);
    latencyLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);
    bufferLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(8);
    errorLabel.setBounds(area.removeFromTop(60));
    area.removeFromTop(12);

    auto row = area.removeFromTop(24);
    inputBufferSizeLabel.setBounds(row.removeFromLeft(100));
    row.removeFromLeft(8);
    inputBufferSizeCombo.setBounds(row.removeFromLeft(160));
    row.removeFromLeft(16);
    outputBufferSizeLabel.setBounds(row.removeFromLeft(105));
    row.removeFromLeft(8);
    outputBufferSizeCombo.setBounds(row.removeFromLeft(160));

    area.removeFromTop(8);
    auto mrow = area.removeFromTop(24);
    measureButton.setBounds(mrow.removeFromLeft(220));
    mrow.removeFromLeft(8);
    measuredLabel.setBounds(mrow);
}