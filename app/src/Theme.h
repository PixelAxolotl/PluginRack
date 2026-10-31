#pragma once

#include <JuceHeader.h>

namespace theme {

inline juce::Colour bg()        { return juce::Colour(0xff1b2229); }
inline juce::Colour panel()     { return juce::Colour(0xff232d36); }
inline juce::Colour strip()     { return juce::Colour(0xff2b3742); }
inline juce::Colour control()   { return juce::Colour(0xff35434f); }
inline juce::Colour border()    { return juce::Colour(0xff465663); }
inline juce::Colour line()      { return juce::Colour(0xff5d7183); }
inline juce::Colour text()      { return juce::Colour(0xffc9d6e0); }
inline juce::Colour dim()       { return juce::Colour(0xff8fa3b3); }
inline juce::Colour green()     { return juce::Colour(0xff7fd4a8); }
inline juce::Colour deepGreen() { return juce::Colour(0xff4a9b74); }
inline juce::Colour red()       { return juce::Colour(0xffe2574c); }
inline juce::Colour amber()     { return juce::Colour(0xffe8b84b); }
inline juce::Colour orange()    { return juce::Colour(0xffe08a3c); }
inline juce::Colour cyan()      { return juce::Colour(0xff4fb3c9); }
inline juce::Colour blue()      { return juce::Colour(0xff3f7ef7); }
inline juce::Colour violet()    { return juce::Colour(0xff8f7fd4); }

// Shared strip metrics: input and output strips are read side by side, so a
// button or field must not change height when it moves between them.
namespace metrics {
inline constexpr int kActionH = 18;  // strip buttons beside the close cross (split/flip)
inline constexpr int kFieldH = 20;   // combos and the input chip
inline constexpr int kRowH = 22;     // mute / solo row
inline constexpr int kDeviceH = 30;  // name + device block
inline constexpr int kActionW = 44;  // split / flip
inline constexpr int kCloseW = 20;   // the cross
} // namespace metrics

} // namespace theme