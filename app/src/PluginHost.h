#pragma once

#include <JuceHeader.h>

class PluginHost {
public:
    PluginHost();
    ~PluginHost();

    juce::String loadPlugin(const juce::File& file);
    void prepareToPlay(double sampleRate, int samplesPerBlock);
    void releaseResources();
    void processBlock(juce::AudioBuffer<float>& buffer, bool bypassed = false);

    juce::String getPluginPath() const;
    juce::String getPluginName() const;
    double getSampleRate() const;
    int getBufferSize() const;

    bool hasPlugin() const;
    int getLatencySamples() const;
    void showEditor();
    void editorBeingDeleted(juce::AudioProcessorEditor* editor);
    void clearEditorWindow(juce::DocumentWindow* window);

private:
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    mutable juce::CriticalSection pluginLock;
    juce::MidiBuffer midiBuffer;
    juce::String pluginPath;
    juce::String pluginName;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 256;

    juce::DocumentWindow* editorWindow = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHost)
};
