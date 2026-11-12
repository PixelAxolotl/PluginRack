#pragma once

namespace pluginrack {

// The opt-in cap the app applies to JUCE's WASAPI channel-count probe.
// Zero means "no cap": the backend keeps its stock 64..default loop.
inline constexpr int kWasapiProbeCapMinChannels = 2;
inline constexpr int kWasapiProbeCapMaxChannels = 64;

int wasapiProbeCapValue(bool enabled, int channels);

} // namespace pluginrack
