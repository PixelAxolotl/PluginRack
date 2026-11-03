#include "SliderTouch.h"

#include <optional>

#include "ValueEditSlider.h"

namespace slider_touch {

namespace {

juce::Component::SafePointer<juce::Slider> lastSlider;
std::optional<Dial> lastDial;
std::function<void()> observer;
bool driving = false;

// The dial a press landed on: the event target itself or, for a press on a
// slider's value box, its nearest slider ancestor.
juce::Slider* sliderAt(const juce::MouseEvent& e) {
    for (auto* c = e.eventComponent; c != nullptr; c = c->getParentComponent())
        if (auto* slider = dynamic_cast<juce::Slider*>(c)) return slider;
    return nullptr;
}

// Reads the leading number out of what the user typed, so "-6 dB" and "1.25"
// both work. False when the text holds no digits at all.
bool parseTypedValue(const juce::String& text, double& outValue) {
    juce::String numeric;
    for (auto character : text.trim()) {
        const auto c = static_cast<juce_wchar>(character);
        if (juce::CharacterFunctions::isDigit(c) || c == '.' || c == ',' || c == '-'
            || c == '+' || c == 'e' || c == 'E') {
            numeric += c == ',' ? '.' : c;
        } else if (numeric.isNotEmpty()) {
            break; // reached the suffix (dB, %, ...)
        }
    }
    if (!numeric.containsAnyOf("0123456789")) return false;
    outValue = numeric.getDoubleValue();
    return true;
}

void showValueBox(juce::Slider& slider) {
    juce::Component::SafePointer<juce::Slider> safe(&slider);
    auto* window = new juce::AlertWindow(
        "Set value",
        "Enter a value between " + juce::String(slider.getMinimum(), 2) + " and "
            + juce::String(slider.getMaximum(), 2),
        juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("value", slider.getTextFromValue(slider.getValue()));
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safe, window](int result) {
            if (result == 1 && safe != nullptr) {
                double typed = 0.0;
                if (parseTypedValue(window->getTextEditorContents("value"), typed))
                    safe->setValue(typed, juce::sendNotificationSync);
            }
            delete window;
        }),
        false);
}

class RightClickEditor final : public juce::MouseListener {
public:
    void mouseDown(const juce::MouseEvent& e) override {
        auto* slider = sliderAt(e);
        if (slider == nullptr || !slider->isEnabled()) return;

        record(*slider);
        if (!e.mods.isPopupMenu()) return;
        // Our own dials open their value box inline on right-click; only dials
        // without one need the floating box.
        if (dynamic_cast<ValueEditSlider*>(slider) != nullptr) return;

        juce::Component::SafePointer<juce::Slider> safe(slider);
        // Defer: the modal field must not open while JUCE is still dispatching
        // the press to the dial.
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr) showValueBox(*safe.getComponent());
        });
    }
};

} // namespace

void record(juce::Slider& slider) {
    lastSlider = &slider;
    juce::Component::SafePointer<juce::Slider> safe(&slider);

    Dial dial;
    dial.name = slider.getName();
    if (dial.name.isEmpty()) {
        const auto tip = slider.getTooltip();
        dial.name = tip.isNotEmpty() ? tip.upToFirstOccurrenceOf("\n", false, false).trim()
                                     : juce::String("Dial");
    }
    dial.getProportion = [safe] {
        if (safe == nullptr) return 0.0;
        return juce::jlimit(0.0, 1.0, safe->valueToProportionOfLength(safe->getValue()));
    };
    dial.setProportion = [safe](double proportion) {
        if (safe == nullptr) return;
        safe->setValue(safe->proportionOfLengthToValue(juce::jlimit(0.0, 1.0, proportion)),
                       juce::sendNotificationSync);
    };
    dial.getText = [safe] {
        return safe == nullptr ? juce::String("--")
                               : safe->getTextFromValue(safe->getValue());
    };
    if (slider.isDoubleClickReturnEnabled()) {
        dial.defaultProportion = juce::jlimit(
            0.0, 1.0, slider.valueToProportionOfLength(slider.getDoubleClickReturnValue()));
    }

    lastDial = std::move(dial);
    if (observer) observer();
}

void recordDial(Dial dial) {
    lastSlider = nullptr;
    lastDial = std::move(dial);
    if (observer) observer();
}

void clear() {
    lastSlider = nullptr;
    lastDial.reset();
    if (observer) observer();
}

const Dial* current() {
    return lastDial.has_value() ? &*lastDial : nullptr;
}

void setObserver(std::function<void()> newObserver) {
    observer = std::move(newObserver);
}

void beginDriving() { driving = true; }
void endDriving() { driving = false; }
bool isDriving() { return driving; }

void installRightClickEditor() {
    static RightClickEditor editor; // outlives the application's windows
    juce::Desktop::getInstance().addGlobalMouseListener(&editor);
}

} // namespace slider_touch
