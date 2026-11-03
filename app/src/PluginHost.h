#pragma once

#include <JuceHeader.h>

#include <atomic>

class PluginHost {
public:
    // Result of instantiating a VST3 file. The instance is shared so it can be
    // built on a worker thread (instantiation blocks for as long as the plugin
    // takes to initialise) and then adopted on the message thread, where
    // prepareToPlay must run: some plugins synchronise with it.
    struct LoadedPlugin {
        std::shared_ptr<juce::AudioPluginInstance> instance;
        juce::String path;
        juce::String name;
        juce::String error;
    };

    PluginHost();
    ~PluginHost();

    // Blocking instantiation. Only for callers that are already off the
    // message thread or can afford the stall; the Add-VST path uses
    // loadFromFile on a worker plus adopt on the message thread.
    static LoadedPlugin loadFromFile(const juce::File& file, double sampleRate,
                                     int bufferSize, const juce::String& stateBase64 = {});
    // Message thread: takes over the instance and prepares it to play.
    void adopt(const LoadedPlugin& loaded);

    // stateBase64 is an opaque blob previously returned by getStateAsBase64;
    // when non-empty it is applied before the plugin is prepared to play, so
    // a restored slot comes back with the settings it was saved with.
    juce::String loadPlugin(const juce::File& file, const juce::String& stateBase64 = {});
    // Captures the plugin's own state (parameters, programs) as base64 text.
    // Empty when no plugin is loaded or the plugin exposes no state.
    juce::String getStateAsBase64() const;
    void prepareToPlay(double sampleRate, int samplesPerBlock);
    void releaseResources();
    void processBlock(juce::AudioBuffer<float>& buffer, bool bypassed = false);

    juce::String getPluginPath() const;
    juce::String getPluginName() const;
    double getSampleRate() const;
    int getBufferSize() const;

    bool hasPlugin() const;
    // Lock-free cached latency: refreshed on load/prepare/bypass so the
    // latency report never blocks on pluginLock.
    int getLatencySamples() const;
    void refreshLatencyCache();
    void showEditor();
    // Channel-view docking: create an editor the caller owns and embeds inline
    // (addAndMakeVisible). Hides any floating window first. Returns null when
    // the plugin has no editor; destroy with destroyDockEditor.
    juce::AudioProcessorEditor* createEditorForDock();
    void destroyDockEditor(juce::AudioProcessorEditor* editor);
    void closeFloatingEditor();
    void editorBeingDeleted(juce::AudioProcessorEditor* editor);
    void clearEditorWindow(juce::DocumentWindow* window);
    void closeEditorWindow(juce::DocumentWindow* window);

private:
    std::shared_ptr<juce::AudioPluginInstance> plugin;
    mutable juce::CriticalSection pluginLock;
    juce::MidiBuffer midiBuffer;
    juce::String pluginPath;
    juce::String pluginName;
    double currentSampleRate = 48000.0;
    int currentBufferSize = 256;
    bool prepared = false;
    std::atomic<int> cachedLatencySamples_{0};

    juce::DocumentWindow* editorWindow = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHost)
};
