#pragma once

#include <JuceHeader.h>

// The audio backend a stream actually opened in. JUCE reports long registered
// names ("Windows Audio (Low Latency Mode)"); the UI needs something a user can
// read at a glance to answer "am I on ASIO or WASAPI right now?".
//
// Kept as one function so the Settings page, the strips and the latency budget
// can never disagree about what a stream is running on.
inline juce::String deviceBackendTag(const juce::String& deviceType) {
    if (deviceType.isEmpty()) return {};
    if (deviceType == "ASIO") return "ASIO";
    if (deviceType == "Windows Audio (Low Latency Mode)") return "WASAPI LL";
    if (deviceType == "Windows Audio (Exclusive Mode)") return "WASAPI excl";
    if (deviceType == "Windows Audio") return "WASAPI";
    if (deviceType == "DirectSound") return "DirectSound";
    return deviceType;
}

// ASIO is the only backend that bypasses the Windows audio engine, so it earns
// the accent colour everywhere it is shown.
inline bool deviceBackendIsAsio(const juce::String& deviceType) {
    return deviceType == "ASIO";
}
