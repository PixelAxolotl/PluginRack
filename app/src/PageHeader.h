#pragma once

#include <JuceHeader.h>

#include "Theme.h"

// The title band every page view shares: title on the left, a status line on
// the right, one hairline under both. Page views reserve ui::kHeaderH for it;
// everything below that line starts at ui::kHeaderGap.
class PageHeader final : public juce::Component {
public:
    explicit PageHeader(const juce::String& titleText) {
        titleLabel.setText(titleText, juce::dontSendNotification);
        titleLabel.setFont(theme::ui::titleFont());
        titleLabel.setColour(juce::Label::textColourId, theme::text());
        addAndMakeVisible(titleLabel);

        statusLabel.setJustificationType(juce::Justification::centredRight);
        statusLabel.setFont(theme::ui::captionFont());
        statusLabel.setColour(juce::Label::textColourId, theme::dim());
        addAndMakeVisible(statusLabel);
    }

    void setTitle(const juce::String& text) {
        titleLabel.setText(text, juce::dontSendNotification);
    }

    void setStatus(const juce::String& text, juce::Colour colour = theme::dim()) {
        statusLabel.setText(text, juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, colour);
    }

    void resized() override {
        auto area = getLocalBounds();
        area.removeFromBottom(1); // hairline
        statusLabel.setBounds(area.removeFromRight(juce::jmax(0, area.getWidth() / 2)));
        titleLabel.setBounds(area);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(theme::border().withAlpha(0.8f));
        g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());
    }

private:
    juce::Label titleLabel;
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PageHeader)
};
