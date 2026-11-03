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
// End inset a vertical fader's track leaves inside its slider area. The meter
// beside it maps to the fader's value region, which is why the two read out at
// the same height.
inline constexpr int kFaderTrackInset = 7;
// dB marks shared by the fader's ticks and the meter's scale, so a mark on
// either side of a fader reads at the same height.
inline constexpr int kDbMarks[] = {12, 6, 0, -3, -6, -10, -15, -20, -30, -40, -50, -60};
} // namespace metrics

// One type scale, one spacing scale and one control vocabulary for every
// surface. Page views (Settings, Saves, Routing, I/O) share the page header;
// the Mixer and Rack are full-bleed workspaces and keep their tighter inset,
// but they draw from the same fonts and control colours so a button or a
// caption never changes its look when it moves between tabs.
namespace ui {

inline constexpr float kTitlePt = 18.0f;   // page title
inline constexpr float kItemPt = 13.0f;    // row / strip title
inline constexpr float kSectionPt = 10.0f; // uppercase section / column caption
inline constexpr float kBodyPt = 12.0f;    // body and field text
inline constexpr float kCaptionPt = 11.0f; // secondary line under a control

inline constexpr int kPagePad = 12;     // page inset for page views
inline constexpr int kHeaderH = 34;     // page title row
inline constexpr int kHeaderGap = 10;   // title row to first section
inline constexpr int kSectionH = 16;    // section caption row
inline constexpr int kSectionGap = 14;  // between sections
inline constexpr int kRowGap = 6;       // between controls on one row
inline constexpr int kControlH = 24;    // standard field / button height
inline constexpr int kMeasureW = 150;   // standard action button width

inline juce::Font titleFont() {
    return juce::Font(juce::FontOptions(kTitlePt, juce::Font::bold));
}
inline juce::Font itemFont() {
    return juce::Font(juce::FontOptions(kItemPt, juce::Font::bold));
}
inline juce::Font sectionFont() {
    return juce::Font(juce::FontOptions(kSectionPt, juce::Font::bold));
}
inline juce::Font bodyFont() {
    return juce::Font(juce::FontOptions(kBodyPt));
}
inline juce::Font bodyBoldFont() {
    return juce::Font(juce::FontOptions(kBodyPt, juce::Font::bold));
}
inline juce::Font captionFont() {
    return juce::Font(juce::FontOptions(kCaptionPt));
}

// Panel fill + hairline, the console's only container treatment.
inline void drawPanel(juce::Graphics& g, juce::Rectangle<int> bounds,
                      juce::Colour fill = panel(), juce::Colour outline = border()) {
    g.setColour(fill);
    g.fillRoundedRectangle(bounds.toFloat(), 3.0f);
    g.setColour(outline);
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 3.0f, 1.0f);
}

// Uppercase caption that names a panel section (INPUTS, INSERTS, ROUND-TRIP).
inline void drawSectionCaption(juce::Graphics& g, juce::Rectangle<int> bounds,
                               const juce::String& text) {
    g.setColour(dim().withAlpha(0.8f));
    g.setFont(sectionFont());
    g.drawText(text.toUpperCase(), bounds, juce::Justification::centredLeft, true);
}

inline void styleButton(juce::TextButton& button) {
    button.setColour(juce::TextButton::buttonColourId, control());
    button.setColour(juce::TextButton::buttonOnColourId, green());
    button.setColour(juce::TextButton::textColourOffId, theme::text());
    button.setColour(juce::TextButton::textColourOnId, bg());
}

inline void styleCombo(juce::ComboBox& combo) {
    combo.setColour(juce::ComboBox::backgroundColourId, control());
    combo.setColour(juce::ComboBox::textColourId, theme::text());
    combo.setColour(juce::ComboBox::arrowColourId, dim());
    combo.setColour(juce::ComboBox::outlineColourId, border());
}

inline void styleToggle(juce::ToggleButton& toggle) {
    toggle.setColour(juce::ToggleButton::textColourId, theme::text());
    toggle.setColour(juce::ToggleButton::tickColourId, green());
    toggle.setColour(juce::ToggleButton::tickDisabledColourId, dim());
}

// Amber BETA tag that sits beside a section caption. Feature is usable, the
// behaviour is still settling, and the label says so instead of hiding it.
inline void styleBetaLabel(juce::Label& label) {
    label.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    label.setColour(juce::Label::textColourId, amber());
    label.setJustificationType(juce::Justification::centredLeft);
}

} // namespace ui

} // namespace theme