#include "TouchSlideBar.h"

#include <cmath>

#include "SliderTouch.h"
#include "Theme.h"

namespace {
constexpr int kCaptionH = 56; // linked-dial name + "TOUCH / AND / SLIDE"
constexpr int kReadoutH = 26;
constexpr int kThumbW = 28;
constexpr int kThumbH = 16;
constexpr int kTickPitch = 7;
// How close (in proportion of the dial's range) the slide must come to the
// dial's default before it snaps onto it.
constexpr double kSnapWindow = 0.015;
} // namespace

TouchSlideBar::TouchSlideBar() {
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    // Follow every newly touched dial: the strip is a helper for whatever the
    // user reached for last, not for the one it happened to drive first.
    slider_touch::setObserver([this] { repaint(); });
    startTimerHz(12); // the dial may also be moved from its own editor
}

TouchSlideBar::~TouchSlideBar() {
    stopTimer();
    slider_touch::setObserver({});
}

juce::Rectangle<int> TouchSlideBar::trackBounds() const {
    auto area = getLocalBounds().reduced(6);
    area.removeFromTop(kCaptionH);
    area.removeFromBottom(kReadoutH);
    // The thumb travels down the middle; the ticks sit either side of it.
    return area.withSizeKeepingCentre(kThumbW * 2, juce::jmax(20, area.getHeight()));
}

void TouchSlideBar::paint(juce::Graphics& g) {
    theme::ui::drawPanel(g, getLocalBounds(), theme::panel().darker(0.08f), theme::border());

    auto* dial = slider_touch::current();
    const bool live = dial != nullptr;

    auto area = getLocalBounds().reduced(6);
    auto caption = area.removeFromTop(kCaptionH);

    // Top line: what the strip is linked to, so a touchscreen user can see
    // which dial the next slide will move.
    g.setColour(live ? theme::amber() : theme::dim().withAlpha(0.65f));
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawFittedText(live ? dial->name : juce::String("--"), caption.removeFromTop(14),
                     juce::Justification::centred, 2);

    g.setColour(theme::dim().withAlpha(0.85f));
    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
    const char* lines[3] = { "TOUCH", "AND", "SLIDE" };
    for (int i = 0; i < 3; ++i)
        g.drawText(lines[i], caption.removeFromTop(12), juce::Justification::centred, false);

    const auto track = trackBounds();

    // Scale: a hairline with fine ticks either side, like a long-throw fader.
    const float centreX = static_cast<float>(track.getCentreX());
    const float top = static_cast<float>(track.getY());
    const float bottom = static_cast<float>(track.getBottom());
    g.setColour(theme::line().withAlpha(live ? 1.0f : 0.4f));
    g.fillRect(juce::Rectangle<float>(centreX - 1.0f, top, 2.0f, bottom - top));
    g.setColour(theme::border().withAlpha(live ? 0.9f : 0.35f));
    for (float y = top + 2.0f; y < bottom - 2.0f; y += static_cast<float>(kTickPitch)) {
        const bool major = (static_cast<int>((y - top) / kTickPitch) % 4) == 0;
        const float half = major ? 9.0f : 5.0f;
        g.drawLine(centreX - half, std::round(y), centreX - 3.0f, std::round(y), 1.0f);
        g.drawLine(centreX + 3.0f, std::round(y), centreX + half, std::round(y), 1.0f);
    }

    // Default marker: a heavier tick at the dial's own default, so the point the
    // slide snaps to (and the double-click returns to) is visible.
    if (live && dial->defaultProportion >= 0.0) {
        const float yDefault =
            bottom - static_cast<float>(dial->defaultProportion) * (bottom - top);
        g.setColour(theme::amber().withAlpha(0.95f));
        g.fillRect(centreX - 13.0f, std::round(yDefault) - 1.5f, 26.0f, 3.0f);
    }

    // Thumb: where the dial currently sits (bottom = minimum, top = maximum).
    const double proportion = live ? juce::jlimit(0.0, 1.0, dial->getProportion()) : 0.0;
    const float thumbY = bottom - static_cast<float>(proportion) * (bottom - top);
    juce::Rectangle<float> thumb(centreX - kThumbW * 0.5f,
                                 juce::jlimit(top, bottom - kThumbH, thumbY - kThumbH * 0.5f),
                                 static_cast<float>(kThumbW), static_cast<float>(kThumbH));
    g.setColour((live ? theme::green() : theme::dim()).withAlpha(live ? 1.0f : 0.5f));
    g.fillRoundedRectangle(thumb, 4.0f);
    g.setColour(theme::bg().withAlpha(0.55f));
    g.fillRect(thumb.getX() + 6.0f, thumb.getCentreY() - 1.0f, thumb.getWidth() - 12.0f, 2.0f);

    // Readout: the dial's own value text, so the strip reads like the dial.
    auto readout = getLocalBounds().reduced(4).removeFromBottom(kReadoutH - 4);
    g.setColour(live ? theme::text() : theme::dim().withAlpha(0.6f));
    g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    g.drawFittedText(live ? dial->getText() : juce::String("--"),
                     readout, juce::Justification::centred, 1);
}

void TouchSlideBar::mouseDown(const juce::MouseEvent& e) {
    auto* dial = slider_touch::current();
    if (dial == nullptr) return;
    dragging = true;
    slider_touch::beginDriving();
    dragStartY = e.getPosition().y;
    dragStartProportion = juce::jlimit(0.0, 1.0, dial->getProportion());
}

void TouchSlideBar::mouseDrag(const juce::MouseEvent& e) {
    if (!dragging) return;
    const auto track = trackBounds();
    const double span = juce::jmax(1.0, static_cast<double>(track.getHeight()));
    const double travelled = static_cast<double>(dragStartY - e.getPosition().y) / span;
    applyProportion(dragStartProportion + travelled);
}

void TouchSlideBar::mouseUp(const juce::MouseEvent&) {
    dragging = false;
    slider_touch::endDriving();
    repaint();
}

void TouchSlideBar::mouseDoubleClick(const juce::MouseEvent&) {
    auto* dial = slider_touch::current();
    if (dial == nullptr || dial->defaultProportion < 0.0) return;
    dragging = false;
    slider_touch::endDriving();
    dial->setProportion(dial->defaultProportion);
    repaint();
}

void TouchSlideBar::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) {
    auto* dial = slider_touch::current();
    if (dial == nullptr) return;
    applyProportion(dial->getProportion() + static_cast<double>(wheel.deltaY) * 0.05);
}

void TouchSlideBar::applyProportion(double proportion) {
    if (auto* dial = slider_touch::current()) {
        double target = juce::jlimit(0.0, 1.0, proportion);
        // Snap onto the default so it can be hit exactly by touch.
        if (dial->defaultProportion >= 0.0
            && std::abs(target - dial->defaultProportion) <= kSnapWindow) {
            target = dial->defaultProportion;
        }
        dial->setProportion(target);
    }
    repaint();
}

void TouchSlideBar::timerCallback() {
    // Repaint while a dial is held so the thumb and readout follow edits made
    // on the dial itself (or by the plugin).
    if (slider_touch::current() != nullptr) repaint();
}
