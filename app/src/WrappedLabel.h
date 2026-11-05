#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>

#include "Theme.h"

// A label that wraps. juce::Label only fits one line and ellipsizes the rest,
// which is wrong for a device error: the sentence says what to change, so it has
// to stay readable. Drawn through TextLayout, and heightFor() lets the layout
// reserve exactly the height the wrap needs.
class WrappedLabel final : public juce::Component {
public:
    WrappedLabel() { setInterceptsMouseClicks(false, false); }

    void setText(const juce::String& newText, juce::Colour newColour,
                 juce::Font newFont = theme::ui::captionFont()) {
        font_ = newFont;
        if (text_ == newText && colour_ == newColour) return;
        text_ = newText;
        colour_ = newColour;
        repaint();
    }

    const juce::String& getText() const noexcept { return text_; }
    juce::Colour getTextColour() const noexcept { return colour_; }
    const juce::Font& getFont() const noexcept { return font_; }

    static int heightFor(const juce::String& text, int width,
                         juce::Font font = theme::ui::captionFont()) {
        if (text.isEmpty() || width <= 0) return 0;
        const auto layout = build(text, width, font, theme::dim());
        return std::max(1, (int) std::ceil(layout.getHeight()) + 1);
    }

    void paint(juce::Graphics& g) override {
        if (text_.isEmpty()) return;
        build(text_, getWidth(), font_, colour_).draw(g, getLocalBounds().toFloat());
    }

private:
    static juce::TextLayout build(const juce::String& text, int width, juce::Font font,
                                  juce::Colour colour) {
        juce::AttributedString attributed;
        // Font and colour apply to the string that is already in it, so the
        // text has to be appended first.
        attributed.append(text);
        attributed.setJustification(juce::Justification::topLeft);
        attributed.setLineSpacing(1.15f);
        attributed.setFont(font);
        attributed.setColour(colour);

        juce::TextLayout layout;
        layout.createLayout(attributed, (float) std::max(1, width));
        return layout;
    }

    juce::String text_;
    juce::Font font_{ theme::ui::captionFont() };
    juce::Colour colour_{ theme::dim() };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WrappedLabel)
};
