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
}

void PluginKnobWatcher::clear() {
    // Detach first: the parameters belong to the processor, so this listener
    // relationship must not outlive it.
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
    // Fallback for plugins that never report a gesture. While a knob is held
    // its gesture owns the strip, and edits made by the strip itself are
    // suppressed so a plugin's linked parameters cannot steal the target.
    post([parameterIndex](PluginKnobWatcher& watcher) {
        if (watcher.gestureIndex >= 0) return;
        if (slider_touch::isDriving()) return;
        if (juce::Time::getMillisecondCounter() < watcher.shared->suppressUntilMs) return;
        watcher.bind(parameterIndex);
    });
}

void PluginKnobWatcher::bind(int parameterIndex) {
    auto* parameter = parameterForIndex(parameterIndex);
    if (parameter == nullptr) return;

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

    slider_touch::recordDial(std::move(dial));
}
