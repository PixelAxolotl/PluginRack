#include "PluginKnobWatcher.h"

#include "SliderTouch.h"

PluginKnobWatcher::~PluginKnobWatcher() {
    clear();
}

void PluginKnobWatcher::setPlugin(juce::AudioPluginInstance* newInstance) {
    if (newInstance == instance) return;
    clear();
    instance = newInstance;
    if (instance == nullptr) return;

    for (auto* parameter : instance->getParameters()) {
        parameters.push_back(parameter);
        if (parameter != nullptr) parameter->addListener(this);
    }
    startTimerHz(20);
}

void PluginKnobWatcher::clear() {
    stopTimer();
    // Detach first: the parameters belong to the processor, so this listener
    // relationship must not outlive it. removeListener() waits for a callback
    // in flight, so nothing can store into pendingValueIndex after this loop.
    for (auto* parameter : parameters)
        if (parameter != nullptr) parameter->removeListener(this);
    parameters.clear();
    instance = nullptr;
    pendingValueIndex.store(-1, std::memory_order_relaxed);
    boundIndex = -1;

    shared->alive = false; // any dial still pointing at a parameter goes inert
    shared = std::make_shared<Shared>();
    gestureIndex = -1;
    slider_touch::clear();
}

void PluginKnobWatcher::post(std::function<void(PluginKnobWatcher&)> action) {
    // Gestures arrive on the plugin's UI thread and value changes can arrive on
    // the audio thread; the strip lives on the message thread.
    auto state = shared;
    juce::MessageManager::callAsync([state, this, action = std::move(action)] {
        if (state->alive) action(*this);
    });
}

juce::AudioProcessorParameter* PluginKnobWatcher::parameterForIndex(int index) const {
    if (index < 0) return nullptr;
    if (index < static_cast<int>(parameters.size())) {
        if (auto* parameter = parameters[static_cast<std::size_t>(index)])
            if (parameter->getParameterIndex() == index) return parameter;
    }
    for (auto* parameter : parameters)
        if (parameter != nullptr && parameter->getParameterIndex() == index) return parameter;
    return nullptr;
}

void PluginKnobWatcher::parameterGestureChanged(int parameterIndex, bool gestureIsStarting) {
    post([parameterIndex, gestureIsStarting](PluginKnobWatcher& watcher) {
        if (gestureIsStarting) {
            // Another knob is already held: this gesture belongs to a parameter
            // the plugin linked to it, not to a knob the user grabbed.
            if (watcher.gestureIndex >= 0) return;
            if (slider_touch::isDriving()) return;
            watcher.gestureIndex = parameterIndex;
            watcher.bind(parameterIndex);
        } else if (watcher.gestureIndex == parameterIndex) {
            watcher.gestureIndex = -1;
        }
    });
}

void PluginKnobWatcher::parameterValueChanged(int parameterIndex, float) {
    // Any thread, including the audio thread: one relaxed store, nothing else.
    pendingValueIndex.store(parameterIndex, std::memory_order_relaxed);
}

void PluginKnobWatcher::timerCallback() {
    // Fallback for plugins that never report a gesture. While a knob is held
    // its gesture owns the strip, and edits made by the strip itself are
    // suppressed so a plugin's linked parameters cannot steal the target.
    // Taken before the checks so a change made during a gesture is dropped.
    const int index = pendingValueIndex.exchange(-1, std::memory_order_relaxed);
    if (index < 0) return;
    if (gestureIndex >= 0) return;
    if (slider_touch::isDriving()) return;
    if (juce::Time::getMillisecondCounter() < shared->suppressUntilMs) return;
    bind(index);
}

void PluginKnobWatcher::bind(int parameterIndex) {
    auto* parameter = parameterForIndex(parameterIndex);
    if (parameter == nullptr) return;
    // Already the strip's target: re-recording it would only rebuild the same
    // dial and repaint the strip.
    if (parameterIndex == boundIndex && slider_touch::generation() == boundGeneration
        && slider_touch::current() != nullptr)
        return;

    auto state = shared;
    slider_touch::Dial dial;
    dial.name = parameter->getName(48).trim();
    if (dial.name.isEmpty()) dial.name = "Plugin Knob";
    dial.getProportion = [state, parameter] {
        if (!state->alive) return 0.0;
        return juce::jlimit(0.0, 1.0, static_cast<double>(parameter->getValue()));
    };
    dial.setProportion = [state, parameter](double proportion) {
        if (!state->alive) return;
        // Our own move: keep it from being mistaken for a knob grab.
        state->suppressUntilMs = juce::Time::getMillisecondCounter() + 300;
        parameter->setValueNotifyingHost(static_cast<float>(juce::jlimit(0.0, 1.0, proportion)));
    };
    dial.getText = [state, parameter] {
        if (!state->alive) return juce::String("--");
        return parameter->getText(parameter->getValue(), 24);
    };
    // VST3 reports each parameter's default alongside its info, and JUCE keeps
    // it on the AudioProcessorParameter.
    dial.defaultProportion =
        juce::jlimit(0.0, 1.0, static_cast<double>(parameter->getDefaultValue()));

    // Stepped/discrete parameters (toggles, mode switches, ...) get their state
    // count so the touch-and-slide strip can snap onto their positions.
    int steps = parameter->getNumSteps();
    if (steps < 2 || steps >= 1000000) steps = 0;
    dial.numSteps = steps;
    dial.steps = steps;
    dial.numStates = steps;
    dial.stateCount = steps;
    dial.states = steps;
    dial.interval = (steps > 1) ? (1.0 / static_cast<double>(steps - 1)) : 0.0;
    dial.getNumSteps = [state, parameter] {
        if (!state->alive) return 0;
        int n = parameter->getNumSteps();
        if (n < 2 || n >= 1000000) return 0;
        return n;
    };

    slider_touch::recordDial(std::move(dial));
}
