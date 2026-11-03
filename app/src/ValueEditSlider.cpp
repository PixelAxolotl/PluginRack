#include "ValueEditSlider.h"

void ValueEditSlider::showValueEditor() {
    if (! isEnabled()) return;

    juce::Component::SafePointer<ValueEditSlider> safe(this);
    juce::MessageManager::callAsync([safe] {
        if (safe == nullptr) return;
        auto* slider = safe.getComponent();
        auto* box = slider->findValueBox();
        if (box == nullptr) return;

        slider->showTextBox();
        if (auto* editor = box->getCurrentTextEditor()) {
            editor->selectAll();
            editor->grabKeyboardFocus();
        }
    });
}

juce::Label* ValueEditSlider::findValueBox() const {
    for (int i = 0; i < getNumChildComponents(); ++i)
        if (auto* label = dynamic_cast<juce::Label*>(getChildComponent(i))) return label;
    return nullptr;
}
