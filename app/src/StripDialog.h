#pragma once

#include <JuceHeader.h>

#include <functional>

#include "Theme.h"

// Shared rename + colour dialog for input and output strips. The name field
// edits the strip name; the swatch row picks the identity hue from the
// curated palette. On OK both are applied through onApply.
class StripDialog final : public juce::Component {
public:
    static void show(const juce::String& title, const juce::String& currentName,
                     float currentHue,
                     std::function<void(const juce::String& newName, float newHue)> onApply);

private:
    class Body final : public juce::Component {
    public:
        Body(const juce::String& currentName, float currentHue,
             std::function<void(const juce::String&, float)> onApply);
        juce::TextEditor& getTextEditor() { return nameEditor; }
        void resized() override;
        void paint(juce::Graphics& g) override;

    private:
        class Swatch final : public juce::Component {
        public:
            float hue = 0.0f;
            bool selected = false;
            std::function<void()> onClicked;
            void mouseDown(const juce::MouseEvent&) override;
            void paint(juce::Graphics& g) override;
        };

        void applyAndClose();
        void closeWindow();
        // Applies a "#RRGGBB" hex entry as the picked colour's hue.
        void applyHexField(const juce::String& text);
        void selectHue(float hue);
        juce::TextEditor nameEditor;
        juce::OwnedArray<Swatch> swatches;
        juce::TextEditor hexEditor;
        juce::TextButton okButton{"OK"};
        juce::TextButton cancelButton{"Cancel"};
        float pickedHue = 0.0f;
        std::function<void(const juce::String&, float)> onApply;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Body)
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StripDialog)
};
