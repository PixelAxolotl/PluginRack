#pragma once

#include <JuceHeader.h>

namespace theme {

// Dark slate-charcoal palette matching the design references: near-black
// surfaces, muted panels, and the four saturated output accents (mint,
// amber, cyan, violet) as the only bright colour on glass. Pink/red carry
// active-state meaning.
inline juce::Colour bg()        { return juce::Colour(0xff0a0c0e); }
inline juce::Colour panel()     { return juce::Colour(0xff15181b); }
inline juce::Colour strip()     { return juce::Colour(0xff1c1f22); }
inline juce::Colour control()   { return juce::Colour(0xff25292d); }
inline juce::Colour border()    { return juce::Colour(0xff34393f); }
inline juce::Colour line()      { return juce::Colour(0xff4a5057); }
inline juce::Colour text()      { return juce::Colour(0xffe8e8e8); }
inline juce::Colour dim()       { return juce::Colour(0xff9a9a9a); }
inline juce::Colour green()     { return juce::Colour(0xff2ee6a8); }
inline juce::Colour deepGreen() { return juce::Colour(0xff1f9d73); }
inline juce::Colour red()       { return juce::Colour(0xffff2d55); }
inline juce::Colour amber()     { return juce::Colour(0xffffa500); }
inline juce::Colour orange()    { return juce::Colour(0xffff8c00); }
inline juce::Colour cyan()      { return juce::Colour(0xff14b8d4); }
inline juce::Colour blue()      { return juce::Colour(0xff38bdf8); }
inline juce::Colour violet()    { return juce::Colour(0xffa855f7); }
inline juce::Colour pink()      { return juce::Colour(0xffec4899); }

// Curated identity palette for per-strip colours (hue degrees). Fifteen hues
// stay distinguishable across a large session; hex entry covers anything else.
inline const std::vector<float>& stripHues() {
    static const std::vector<float> hues{160.0f, 36.0f, 190.0f, 270.0f, 330.0f,
                                          115.0f, 8.0f, 215.0f, 55.0f, 290.0f,
                                          145.0f, 20.0f, 200.0f, 255.0f, 95.0f};
    return hues;
}

inline juce::Colour stripColour(float hueDegrees) {
    return juce::Colour::fromHSV(hueDegrees / 360.0f, 0.75f, 0.95f, 1.0f);
}

inline float randomStripHue(juce::Random& rng) {
    const auto& hues = stripHues();
    return hues[static_cast<size_t>(rng.nextInt(static_cast<int>(hues.size())))];
}

// The hue a colour maps to is derived from its own value; any colour the
// user enters as hex becomes "hue of that colour" on the next random pick,
// so storage stays a single hue float.
inline float hueOfColour(juce::Colour colour) {
    const float r = static_cast<float>(colour.getRed()) / 255.0f;
    const float g = static_cast<float>(colour.getGreen()) / 255.0f;
    const float b = static_cast<float>(colour.getBlue()) / 255.0f;
    const float mx = juce::jmax(r, g, b);
    const float mn = juce::jmin(r, g, b);
    const float d = mx - mn;
    if (d <= 1.0e-6f) return 0.0f;
    float h = 0.0f;
    if (mx == r) {
        h = (g - b) / d;
        if (h < 0.0f) h += 6.0f;
    }
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h *= 60.0f;
    if (h < 0.0f) h += 360.0f;
    return h;
}

// Strip identity accent: a 3px bar across the *bottom* edge of the given
// bounds, inset by the corner radius so it follows a rounded panel and
// never clips under the header buttons. Coordinates come from the bounds
// itself, so callers can pass a sub-rectangle (not just the full size).
inline void drawStripAccent(juce::Graphics& g, juce::Rectangle<int> bounds, float hueDegrees) {
    constexpr float kCorner = 6.0f;
    const auto accent = stripColour(hueDegrees);
    g.setColour(accent);
    const auto bar = juce::Rectangle<float>(static_cast<float>(bounds.getX()) + kCorner,
                                            static_cast<float>(bounds.getBottom() - 4),
                                            static_cast<float>(bounds.getWidth() - 12),
                                            3.0f);
    g.fillRoundedRectangle(bar, 1.5f);
}

// Strip fill: the shared rounded face for every channel strip and the
// Rack left panel, so edges always round identically.
inline void drawStripFace(juce::Graphics& g, juce::Rectangle<int> bounds) {
    constexpr float kCorner = 6.0f;
    g.setColour(strip());
    g.fillRoundedRectangle(bounds.toFloat(), kCorner);
    g.setColour(border());
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), kCorner, 1.0f);
}

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
inline constexpr int kPanelPad = 12;    // inset inside a captioned panel
inline constexpr int kLabelW = 104;     // label column shared by settings rows

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