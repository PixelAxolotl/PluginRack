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

    bufferSizeLabel.setText("Buffer size", juce::dontSendNotification);
    bufferSizeLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    bufferSizeLabel.setColour(juce::Label::textColourId, theme::text());
    addAndMakeVisible(bufferSizeLabel);

    bufferSizeCombo.addItem("Auto (driver default)", 0);
    for (int s : {32, 64, 128, 256, 512, 1024, 2048}) {
        bufferSizeCombo.addItem(juce::String(s) + " samples", s);
    }
    const int cur = audioEngine.getLatencyReport().valid ? audioEngine.getLatencyReport().inputBufferSize : 0;
    bufferSizeCombo.setSelectedId(cur, juce::dontSendNotification);
    if (bufferSizeCombo.getSelectedId() == 0 && cur == 0) bufferSizeCombo.setSelectedId(0, juce::dontSendNotification);
    bufferSizeCombo.setColour(juce::ComboBox::backgroundColourId, theme::control());
    bufferSizeCombo.setColour(juce::ComboBox::textColourId, theme::text());
    bufferSizeCombo.setColour(juce::ComboBox::arrowColourId, theme::dim());
    bufferSizeCombo.setColour(juce::ComboBox::outlineColourId, theme::border());
    bufferSizeCombo.onChange = [this] {
        const int size = bufferSizeCombo.getSelectedId();
        const auto err = audioEngine.setDeviceBufferSize(size);
        refresh();
        if (err.isNotEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
        }
    };
    addAndMakeVisible(bufferSizeCombo);

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
        if (!bufferSizeCombo.isPopupActive()) {
            const int actual = report.outputBufferSize > 0 ? report.outputBufferSize : report.inputBufferSize;
            if (actual == 0) {
                bufferSizeCombo.setSelectedId(0, juce::dontSendNotification);
            } else if (actual == 32 || actual == 64 || actual == 128 || actual == 256
                       || actual == 512 || actual == 1024 || actual == 2048) {
                bufferSizeCombo.setSelectedId(actual, juce::dontSendNotification);
            }
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
    bufferSizeLabel.setBounds(row.removeFromLeft(100));
    row.removeFromLeft(8);
    bufferSizeCombo.setBounds(row.removeFromLeft(160));

    area.removeFromTop(8);
    auto mrow = area.removeFromTop(24);
    measureButton.setBounds(mrow.removeFromLeft(220));
    mrow.removeFromLeft(8);
    measuredLabel.setBounds(mrow);
}