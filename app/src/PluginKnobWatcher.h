#pragma once

#include <JuceHeader.h>

#include <memory>
#include <vector>

// Follows the knobs the user grabs inside a VST's own editor window so the
// rack's TOUCH AND SLIDE strip can drive them.
//
// A plugin's UI lives in its own native child window, so JUCE never sees those
// clicks. What it does see is the parameter gesture: VST3 plugins call
// beginEdit() as soon as a knob is grabbed (before any movement), and JUCE
// forwards that to AudioProcessorParameter::Listener. That makes a plain touch
// register the knob immediately; the value-change callback is only a fallback
// for plugins that never send gestures.
class PluginKnobWatcher final : private juce::AudioProcessorParameter::Listener {
public:
    PluginKnobWatcher() = default;
    ~PluginKnobWatcher() override;

    // The instance is not owned here: call clear() before it is destroyed.
    void setPlugin(juce::AudioPluginInstance* instance);
    void clear();

private:
    struct Shared {
        bool alive = true;
        juce::uint32 suppressUntilMs = 0;
    };

    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override;

    void post(std::function<void(PluginKnobWatcher&)> action);
    juce::AudioProcessorParameter* parameterForIndex(int index) const;
    void bind(int parameterIndex);

    juce::AudioPluginInstance* instance = nullptr; // never owned
    std::vector<juce::AudioProcessorParameter*> parameters;
    int gestureIndex = -1; // >= 0 while a knob is held in the plugin's own UI
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginKnobWatcher)
};
