#include "ConsoleLookAndFeel.h"

#include "Theme.h"

ConsoleLookAndFeel::ConsoleLookAndFeel() {
    using namespace theme;
    setColour(juce::PopupMenu::backgroundColourId, panel());
    setColour(juce::PopupMenu::textColourId, text());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, border());
    setColour(juce::PopupMenu::highlightedTextColourId, bg());
    setColour(juce::PopupMenu::headerTextColourId, dim());

    setColour(juce::AlertWindow::backgroundColourId, panel());
    setColour(juce::AlertWindow::textColourId, text());
    setColour(juce::AlertWindow::outlineColourId, border());

    setColour(juce::TooltipWindow::backgroundColourId, panel());
    setColour(juce::TooltipWindow::textColourId, text());
    setColour(juce::TooltipWindow::outlineColourId, border());
}

void ConsoleLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPosProportional, float rotaryStartAngle,
                                          float rotaryEndAngle, juce::Slider& slider) {
    const float diameter = juce::jmax(1.0f, juce::jmin((float)width, (float)height) - 10.0f);
    const auto bounds = juce::Rectangle<float>(
                            (float)x + ((float)width - diameter) * 0.5f,
                            (float)y + ((float)height - diameter) * 0.5f,
                            diameter, diameter);
    const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float arcRadius = radius - 3.0f;
    const float angle = rotaryStartAngle
                        + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    const auto outline = slider.findColour(juce::Slider::rotarySliderOutlineColourId);
    const auto fill = slider.findColour(juce::Slider::rotarySliderFillColourId);

    g.setColour(theme::bg().brighter(0.08f));
    g.fillEllipse(bounds);
    g.setColour(outline.withAlpha(0.9f));
    g.drawEllipse(bounds, 1.0f);

    juce::Path track;
    track.addArc(centre.x - arcRadius, centre.y - arcRadius, arcRadius * 2.0f, arcRadius * 2.0f,
                 rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(theme::control().brighter(0.15f));
    g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    juce::Path active;
    active.addArc(centre.x - arcRadius, centre.y - arcRadius, arcRadius * 2.0f, arcRadius * 2.0f,
                  rotaryStartAngle, angle, true);
    g.setColour(fill);
    g.strokePath(active, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    g.setColour(theme::panel().brighter(0.22f));
    g.fillEllipse(centre.x - radius * 0.55f, centre.y - radius * 0.55f,
                  radius * 1.1f, radius * 1.1f);
    g.setColour(theme::border().brighter(0.15f));
    g.drawEllipse(centre.x - radius * 0.55f, centre.y - radius * 0.55f,
                  radius * 1.1f, radius * 1.1f, 1.0f);

    juce::Line<float> indicator(centre.x, centre.y - radius * 0.42f,
                                centre.x, centre.y - radius * 0.68f);
    indicator.applyTransform(juce::AffineTransform::rotation(angle, centre.x, centre.y));
    g.setColour(fill.brighter(0.2f));
    g.drawLine(indicator, 2.0f);
}

void ConsoleLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           juce::Slider::SliderStyle style, juce::Slider& slider) {
    if (style != juce::Slider::LinearVertical) {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos,
                                         maxSliderPos, style, slider);
        return;
    }

    const float centreX = x + width * 0.5f;
    const float trackTop = (float)y + 7.0f;
    const float trackBottom = (float)(y + height) - 7.0f;
    const float trackWidth = 7.0f;
    const auto trackColour = slider.findColour(juce::Slider::backgroundColourId);
    const auto fillColour = slider.findColour(juce::Slider::trackColourId);
    const auto thumbColour = slider.findColour(juce::Slider::thumbColourId);

    g.setColour(trackColour.darker(0.35f));
    g.fillRoundedRectangle(centreX - trackWidth * 0.5f, trackTop,
                           trackWidth, trackBottom - trackTop, 3.5f);
    g.setColour(theme::border().withAlpha(0.8f));
    g.drawRoundedRectangle(centreX - trackWidth * 0.5f, trackTop,
                           trackWidth, trackBottom - trackTop, 3.5f, 1.0f);

    g.setColour(fillColour);
    g.fillRoundedRectangle(centreX - 2.0f, sliderPos, 4.0f, trackBottom - sliderPos, 2.0f);

    g.setColour(theme::line().withAlpha(0.55f));
    constexpr int dbMarks[] = {12, 0, -12, -30, -60};
    // Ticks sit on the same dB marks as the labels: an even split implied a
    // linear scale the fader does not use.
    for (const int db : dbMarks) {
        const float proportion = 1.0f - (float)(db + 60) / 72.0f;
        const float tickY = trackTop + (trackBottom - trackTop) * proportion;
        const float tickWidth = db == 0 ? 10.0f : 6.0f;
        g.drawHorizontalLine(juce::roundToInt(tickY), centreX - tickWidth,
                             centreX - trackWidth * 0.5f - 3.0f);
    }
    g.setFont(juce::Font(juce::FontOptions(8.0f)));
    g.setColour(theme::dim().withAlpha(0.9f));

    const float thumbWidth = juce::jmin(34.0f, (float)width - 4.0f);
    const float thumbHeight = 12.0f;
    g.setColour(theme::panel().brighter(0.18f));
    g.fillRoundedRectangle(centreX - thumbWidth * 0.5f, sliderPos - thumbHeight * 0.5f,
                           thumbWidth, thumbHeight, 3.0f);
    g.setColour(thumbColour);
    g.drawRoundedRectangle(centreX - thumbWidth * 0.5f, sliderPos - thumbHeight * 0.5f,
                           thumbWidth, thumbHeight, 3.0f, 1.5f);
    g.drawHorizontalLine(juce::roundToInt(sliderPos), centreX - thumbWidth * 0.34f,
                         centreX + thumbWidth * 0.34f);

    // Scale after the handle and clear of its edge: the handle used to sit on
    // top of the "0" mark, and the end marks were clipped by the bounds.
    g.setColour(theme::dim().withAlpha(0.9f));
    for (const int db : dbMarks) {
        const float proportion = 1.0f - (float)(db + 60) / 72.0f;
        const float markY = trackTop + (trackBottom - trackTop) * proportion;
        const int labelTop = juce::jlimit(y, y + juce::jmax(0, height - 10),
                                          juce::roundToInt(markY - 5.0f));
        g.drawText(juce::String(db), (int)(centreX + thumbWidth * 0.5f + 4.0f), labelTop, 24, 10,
                   juce::Justification::centredLeft, false);
    }
}
