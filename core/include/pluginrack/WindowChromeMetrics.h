#pragma once

#include <string_view>

namespace pluginrack::windowChrome {

// Every built-in editor shows one short, all-caps, one-word title in the
// same top-left slot. json-tag tests lock the set; all five names must
// share the title cell — no longer name with a different width or style.
inline constexpr std::string_view kEqTitle = "EQ";
inline constexpr std::string_view kDynamicsTitle = "DYNAMICS";
inline constexpr std::string_view kMultibandTitle = "MULTIBAND";
inline constexpr std::string_view kDeEsserTitle = "DE-ESSER";
inline constexpr std::string_view kLimiterTitle = "LIMITER";

// Shared layout constants for every built-in plugin window (EQ, Dynamics,
// De-esser, Multiband, Limiter). One header, one set of values, so a header
// or knob row never has a different height from one window to the next.

inline constexpr int kWindowPad = 8;       // outer inset from window edge to content
inline constexpr int kHeaderHeight = 36;   // encased top bar height
inline constexpr int kHeaderSidePad = 12;  // inner side padding of the top bar
inline constexpr int kHeaderGap = 6;       // gap between top bar and analyzer row
inline constexpr int kAnalyzerHeight = 28; // analyzer control row height
inline constexpr int kAnalyzerGap = 8;     // gap between analyzer row and graph, and between graph and knobs
inline constexpr int kKnobRowHeight = 132; // knob row height in every window

inline constexpr float kTitleFontPt = 15.0f; // title text size
inline constexpr int kTitleCellW = 250;      // title cell width
inline constexpr int kTitleMaxChars = 40;    // titles longer than this are truncated

inline constexpr int kEnableButtonW = 72;    // Enable button width
inline constexpr int kPresetComboW = 120;    // preset box width
inline constexpr int kAnalyzerLabelW = 60;   // "Analyzer" label width
inline constexpr int kWeightingButtonW = 24; // Z / A / C button width
inline constexpr int kStepperButtonW = 28;   // + / - button width
inline constexpr int kOffsetLabelW = 34;     // RTA offset value width

} // namespace pluginrack::windowChrome
