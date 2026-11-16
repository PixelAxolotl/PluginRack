#pragma once

#include <JuceHeader.h>

#include <pluginrack/DeviceTypePolicy.h>

// The audio backend a stream actually opened in. JUCE reports long registered
// names ("Windows Audio (Low Latency Mode)"); the UI needs something a user can
// read at a glance to answer "am I on ASIO or WASAPI right now?".
//
// The names and their labels live in DeviceTypePolicy so the opener, the strips
// and the latency budget can never disagree about what a stream is running on.
inline juce::String deviceBackendTag(const juce::String& deviceType) {
    if (deviceType.isEmpty()) return {};
    const auto backend = pluginrack::deviceBackendOf(deviceType.toRawUTF8());
    const auto tag = pluginrack::deviceBackendTag(backend);
    return tag.empty() ? deviceType : juce::String(tag.data(), static_cast<int>(tag.size()));
}

// ASIO is the only backend that bypasses the Windows audio engine, so it earns
// the accent colour everywhere it is shown.
inline bool deviceBackendIsAsio(const juce::String& deviceType) {
    return pluginrack::deviceBackendOf(deviceType.toRawUTF8()) ==
           pluginrack::DeviceBackend::Asio;
}
