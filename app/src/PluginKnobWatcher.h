#pragma once

#include <JuceHeader.h>

#include <memory>
#include <vector>

// Follows the knobs the user grabs inside a VST's own editor window so the
// rack's TOUCH AND SLIDE strip can drive them.
//
// PARKED while editors live out-of-process: there is no in-process instance
// to observe, so setPlugin is a no-op and the strip stays inert. The class
// (and its Listener base) stays so the native-reparent follow-up can rewire
// it to the host's parameter channel without touching every caller.
class PluginKnobWatcher final : private juce::AudioProcessorParameter::Listener {
public:
    PluginKnobWatcher() = default;
    ~PluginKnobWatcher() override;

    // Parked no-op: kept so callers keep compiling.
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
