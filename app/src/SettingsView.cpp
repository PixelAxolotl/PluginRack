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

    for (int s : {64, 128, 256, 512, 1024, 2048}) {
        bufferSizeCombo.addItem(juce::String(s) + " samples", s);
    }
    bufferSizeCombo.setSelectedId(256, juce::dontSendNotification);
    bufferSizeCombo.setColour(juce::ComboBox::backgroundColourId, theme::control());
    bufferSizeCombo.setColour(juce::ComboBox::textColourId, theme::text());
    bufferSizeCombo.setColour(juce::ComboBox::arrowColourId, theme::dim());
    bufferSizeCombo.setColour(juce::ComboBox::outlineColourId, theme::border());
    bufferSizeCombo.onChange = [this] {
        const int size = bufferSizeCombo.getSelectedId();
        if (size <= 0) return;
        const auto err = audioEngine.setDeviceBufferSize(size);
        refresh();
        if (err.isNotEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", err);
        }
    };
    addAndMakeVisible(bufferSizeCombo);

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
    } else {
        latencyLabel.setText("Latency (est.): n/a", juce::dontSendNotification);
        bufferLabel.setText("Sample rate: n/a", juce::dontSendNotification);
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
}