#include "StripDialog.h"

#include <cmath>

namespace {
constexpr int kDialogW = 380;
constexpr int kDialogH = 286;

class StripDialogWindow final : public juce::DocumentWindow {
public:
    explicit StripDialogWindow(const juce::String& title)
        : juce::DocumentWindow(title, theme::panel(),
                               juce::DocumentWindow::closeButton, true) {
        setResizable(false, false);
        setName(title);
        setSize(kDialogW, kDialogH);
    }
    void closeButtonPressed() override {
        juce::Component::SafePointer<StripDialogWindow> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr) delete safe;
        });
    }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StripDialogWindow)
};
} // namespace

void StripDialog::show(const juce::String& title, const juce::String& currentName,
                       float currentHue,
                       std::function<void(const juce::String& newName, float newHue)> onApply) {
    auto* window = new StripDialogWindow(title);
    auto* body = new Body(currentName, currentHue, std::move(onApply));
    window->setContentOwned(body, false);
    window->setSize(kDialogW, kDialogH);
    window->centreWithSize(kDialogW, kDialogH);
    window->setVisible(true);
    window->toFront(true);
    body->getTextEditor().grabKeyboardFocus();
}

StripDialog::Body::Body(const juce::String& currentName, float currentHue,
                        std::function<void(const juce::String&, float)> apply)
    : pickedHue(currentHue), onApply(std::move(apply)) {
    nameEditor.setText(currentName, juce::dontSendNotification);
    nameEditor.setReturnKeyStartsNewLine(false);
    nameEditor.setMultiLine(false);
    nameEditor.onReturnKey = [this] { applyAndClose(); };
    addAndMakeVisible(nameEditor);

    for (const float hue : theme::stripHues()) {
        auto* sw = swatches.add(new Swatch());
        sw->hue = hue;
        sw->selected = std::fabs(hue - currentHue) < 1.0f;
        sw->onClicked = [this, hue] { selectHue(hue); };
        addAndMakeVisible(sw);
    }

    hexEditor.setText("#" + theme::stripColour(currentHue).toString().substring(2).toUpperCase(),
                      juce::dontSendNotification);
    hexEditor.setReturnKeyStartsNewLine(false);
    hexEditor.setMultiLine(false);
    hexEditor.setTooltip("Hex colour, e.g. #ff6ec7");
    hexEditor.onReturnKey = [this] { applyHexField(hexEditor.getText()); };
    addAndMakeVisible(hexEditor);

    okButton.onClick = [this] { applyAndClose(); };
    cancelButton.onClick = [this] { closeWindow(); };
    theme::ui::styleButton(okButton);
    addAndMakeVisible(okButton);
    addAndMakeVisible(cancelButton);
}

void StripDialog::Body::closeWindow() {
    if (auto* top = findParentComponentOfClass<juce::DocumentWindow>()) {
        top->closeButtonPressed();
    }
}

void StripDialog::Body::applyAndClose() {
    const auto newName = nameEditor.getText();
    if (onApply) onApply(newName, pickedHue);
    closeWindow();
}

void StripDialog::Body::selectHue(float hue) {
    pickedHue = hue;
    hexEditor.setText("#" + theme::stripColour(hue).toString().substring(2).toUpperCase(),
                      juce::dontSendNotification);
    for (auto* s : swatches) s->selected = std::fabs(s->hue - hue) < 1.0f;
    repaint();
}

void StripDialog::Body::applyHexField(const juce::String& text) {
    auto trimmed = text.trim().toUpperCase();
    if (trimmed.startsWith("#")) trimmed = trimmed.substring(1);
    if (trimmed.length() != 6) return;
    const auto colour = juce::Colour::fromString("ff" + trimmed);
    if (!colour.isOpaque()) return;
    selectHue(theme::hueOfColour(colour));
}

void StripDialog::Body::resized() {
    auto area = getLocalBounds().reduced(16);
    auto nameRow = area.removeFromTop(28);
    nameEditor.setBounds(nameRow);
    area.removeFromTop(6);
    // Two rows of swatches: 15 hues, 8 on the first row, 7 on the second.
    constexpr int kSwatch = 26;
    constexpr int kGap = 10;
    const int cols = 8;
    const int x0 = area.getX();
    const int y0 = area.getY();
    for (int i = 0; i < swatches.size(); ++i) {
        const int col = i % cols;
        const int row = i / cols;
        swatches[i]->setBounds(x0 + col * (kSwatch + kGap), y0 + row * (kSwatch + kGap),
                               kSwatch, kSwatch);
    }
    area.removeFromTop(2 * (kSwatch + kGap));
    area.removeFromTop(6);
    auto hexRow = area.removeFromTop(28);
    hexEditor.setBounds(hexRow.removeFromRight(110));
    area.removeFromTop(12);
    auto buttonRow = area.removeFromTop(26);
    buttonRow.removeFromRight(10);
    okButton.setBounds(buttonRow.removeFromRight(70));
    buttonRow.removeFromRight(8);
    cancelButton.setBounds(buttonRow.removeFromRight(70));
}

void StripDialog::Body::paint(juce::Graphics& g) {
    g.fillAll(theme::panel());
}

void StripDialog::Body::Swatch::mouseDown(const juce::MouseEvent&) {
    if (onClicked) onClicked();
    repaint();
}

void StripDialog::Body::Swatch::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(theme::stripColour(hue));
    g.fillRoundedRectangle(bounds, 5.0f);
    if (selected) {
        g.setColour(theme::text());
        g.drawRoundedRectangle(bounds.reduced(-1.5f), 5.0f, 2.0f);
    } else {
        g.setColour(theme::border());
        g.drawRoundedRectangle(bounds, 5.0f, 1.0f);
    }
}
