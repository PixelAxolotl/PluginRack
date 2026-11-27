#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>
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
//
// Value changes arrive at audio rate (JUCE reports a VST3's output parameter
// changes from the audio thread), so they are only recorded in an atomic and
// picked up by a message-thread timer: no allocation or message post per
// change, and a plugin that moves parameters constantly cannot flood the UI.
class PluginKnobWatcher final : private juce::AudioProcessorParameter::Listener,
                                private juce::Timer {
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
    void timerCallback() override;

    void post(std::function<void(PluginKnobWatcher&)> action);
    juce::AudioProcessorParameter* parameterForIndex(int index) const;
    void bind(int parameterIndex);

    juce::AudioPluginInstance* instance = nullptr; // never owned
    std::vector<juce::AudioProcessorParameter*> parameters;
    int gestureIndex = -1; // >= 0 while a knob is held in the plugin's own UI
    // Last parameter whose value moved, from any thread; -1 when none.
    std::atomic<int> pendingValueIndex{ -1 };
    // What bind() last recorded, so an unchanged target is not re-recorded.
    int boundIndex = -1;
    std::uint32_t boundGeneration = 0;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginKnobWatcher)
};
