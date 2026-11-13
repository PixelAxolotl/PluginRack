#pragma once

#include <juce_core/juce_core.h>

namespace pluginrack::endpointformat {

// Windows Audio shared mode runs at the endpoint's own default format, and a
// driver that lists only 44.1 kHz ignores every request for the engine rate:
// AudioDeviceManager clamps the ask to the advertised list. Rewriting the
// endpoint's default format makes the next open land on `sampleRate` without
// taking the device exclusively. Returns an empty string on success, otherwise
// why the format could not be changed. No-op on non-Windows builds.
juce::String setDefaultSampleRate (const juce::String& endpointName,
                                   bool wantInput,
                                   double sampleRate);

} // namespace pluginrack::endpointformat
