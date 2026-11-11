#include "PluginKnobWatcher.h"

#include "SliderTouch.h"

PluginKnobWatcher::~PluginKnobWatcher() {
    clear();
}

void PluginKnobWatcher::setPlugin(juce::AudioPluginInstance* newInstance) {
    // Parked: no in-process instance exists while editors live out-of-process.
    juce::ignoreUnused(newInstance);
    clear();
}

void PluginKnobWatcher::clear() {
    // Parked: parameters is always empty while editors live out-of-process,
    // but keep the detach loop so a future rewire cannot leak a listener.
    for (auto* parameter : parameters)
        if (parameter != nullptr) parameter->removeListener(this);
    parameters.clear();
    instance = nullptr;

    shared->alive = false; // any dial still pointing at a parameter goes inert
    shared = std::make_shared<Shared>();
    gestureIndex = -1;
    slider_touch::clear();
}

void PluginKnobWatcher::post(std::function<void(PluginKnobWatcher&)> action) {
    // Parked: no gestures arrive while editors live out-of-process.
    juce::ignoreUnused(action);
}

juce::AudioProcessorParameter* PluginKnobWatcher::parameterForIndex(int index) const {
    juce::ignoreUnused(index);
    return nullptr;
}

void PluginKnobWatcher::parameterGestureChanged(int parameterIndex, bool gestureIsStarting) {
    juce::ignoreUnused(parameterIndex, gestureIsStarting);
}

void PluginKnobWatcher::parameterValueChanged(int parameterIndex, float) {
    juce::ignoreUnused(parameterIndex);
}

void PluginKnobWatcher::bind(int parameterIndex) {
    juce::ignoreUnused(parameterIndex);
}
