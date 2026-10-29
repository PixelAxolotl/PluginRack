#include "PluginHost.h"

namespace {
class PluginWindow final : public juce::DocumentWindow {
public:
    PluginWindow(PluginHost& ownerRef, juce::AudioProcessorEditor* editorRef)
        : juce::DocumentWindow(editorRef != nullptr ? editorRef->getName() : juce::String("Plugin"),
                               juce::Colours::lightgrey, juce::DocumentWindow::allButtons),
          owner(ownerRef), editor(editorRef) {
        setUsingNativeTitleBar(true);
        setContentOwned(editorRef, true);
        setResizable(true, true);
        setResizeLimits(200, 150, 10000, 10000);

        int width = 320;
        int height = 240;
        if (editorRef != nullptr) {
            width = juce::jmax(240, editorRef->getWidth());
            height = juce::jmax(180, editorRef->getHeight());
        }
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
            auto area = display->userBounds;
            width = juce::jmin(width, juce::jmax(200, (int)area.getWidth() - 40));
            height = juce::jmin(height, juce::jmax(150, (int)area.getHeight() - 40));
        }
        centreWithSize(width, height);
        setVisible(true);
    }

    void closeButtonPressed() override {
        // Deleting a window from inside its own close-button callback is fragile:
        // defer so the button can finish dispatching first. The owner may destroy
        // the window before the message arrives, so guard with a weak pointer.
        juce::Component::SafePointer<PluginWindow> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr) safe->owner.closeEditorWindow(safe.getComponent());
        });
    }

    ~PluginWindow() override {
        owner.editorBeingDeleted(editor);
        owner.clearEditorWindow(this);
        editor = nullptr;
    }

private:
    PluginHost& owner;
    juce::AudioProcessorEditor* editor = nullptr;
};
} // namespace

PluginHost::PluginHost() {
    formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());
}

PluginHost::~PluginHost() {
    delete editorWindow;
    editorWindow = nullptr;
}

bool PluginHost::hasPlugin() const {
    const juce::ScopedLock lock(pluginLock);
    return plugin != nullptr;
}

int PluginHost::getLatencySamples() const {
    return cachedLatencySamples_.load(std::memory_order_relaxed);
}

void PluginHost::refreshLatencyCache() {
    const juce::ScopedTryLock lock(pluginLock);
    if (!lock.isLocked()) return;
    cachedLatencySamples_.store(plugin != nullptr ? plugin->getLatencySamples() : 0,
                                std::memory_order_relaxed);
}

void PluginHost::editorBeingDeleted(juce::AudioProcessorEditor* ed) {
    const juce::ScopedLock lock(pluginLock);
    if (plugin != nullptr && ed != nullptr) {
        plugin->editorBeingDeleted(ed);
    }
}

void PluginHost::clearEditorWindow(juce::DocumentWindow* window) {
    if (editorWindow == window) editorWindow = nullptr;
}

void PluginHost::closeEditorWindow(juce::DocumentWindow* window) {
    if (window == nullptr || window != editorWindow) return;
    auto* doomed = editorWindow;
    editorWindow = nullptr;
    delete doomed;
}

void PluginHost::showEditor() {
    if (!hasPlugin()) return;

    if (editorWindow != nullptr) {
        editorWindow->toFront(true);
        return;
    }

    juce::AudioProcessorEditor* ed = nullptr;
    {
        const juce::ScopedLock lock(pluginLock);
        if (plugin != nullptr && plugin->hasEditor()) {
            ed = plugin->createEditorAndMakeActive();
        }
    }

    if (ed == nullptr) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "PluginRack",
                                              "This plugin has no editor.");
        return;
    }

    auto* window = new PluginWindow(*this, ed);
    editorWindow = window;
}

juce::String PluginHost::loadPlugin(const juce::File& file) {
    if (!file.exists()) {
        return "The selected VST3 does not exist.";
    }

    double sampleRate = 0.0;
    int bufferSize = 0;
    {
        const juce::ScopedLock lock(pluginLock);
        sampleRate = currentSampleRate;
        bufferSize = currentBufferSize;
    }

    juce::OwnedArray<juce::PluginDescription> descriptions;
    formatManager.getFormat(0)->findAllTypesForFile(descriptions, file.getFullPathName());
    if (descriptions.isEmpty()) {
        return "No VST3 plugin was found in the selected file.";
    }

    juce::String errorMessage;
    auto candidate = formatManager.createPluginInstance(*descriptions[0], sampleRate, bufferSize,
                                                        errorMessage);
    if (candidate == nullptr) {
        return errorMessage.isNotEmpty() ? errorMessage : "The VST3 could not be instantiated.";
    }

    auto layout = candidate->getBusesLayout();
    if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty()) {
        return "The selected plugin is not an audio effect with input and output buses.";
    }

    layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (!candidate->setBusesLayout(layout)) {
        return "The selected plugin does not support stereo input and output.";
    }

    candidate->setRateAndBufferSizeDetails(sampleRate, bufferSize);
    candidate->prepareToPlay(sampleRate, bufferSize);

    const juce::ScopedLock lock(pluginLock);
    if (plugin != nullptr) {
        plugin->releaseResources();
    }
    pluginPath = file.getFullPathName();
    pluginName = candidate->getName();
    plugin = std::move(candidate);
    cachedLatencySamples_.store(plugin->getLatencySamples(), std::memory_order_relaxed);
    return {};
}

void PluginHost::prepareToPlay(double sampleRate, int samplesPerBlock) {
    const juce::ScopedLock lock(pluginLock);
    currentSampleRate = sampleRate;
    currentBufferSize = samplesPerBlock;
    if (plugin != nullptr) {
        plugin->setRateAndBufferSizeDetails(sampleRate, samplesPerBlock);
        plugin->prepareToPlay(sampleRate, samplesPerBlock);
    }
    cachedLatencySamples_.store(plugin != nullptr ? plugin->getLatencySamples() : 0,
                                std::memory_order_relaxed);
}

void PluginHost::releaseResources() {
    const juce::ScopedLock lock(pluginLock);
    if (plugin != nullptr) {
        plugin->releaseResources();
    }
}

void PluginHost::processBlock(juce::AudioBuffer<float>& buffer, bool bypassed) {
    if (bypassed) {
        return;
    }

    const juce::ScopedTryLock lock(pluginLock);
    if (!lock.isLocked() || plugin == nullptr) {
        return;
    }

    midiBuffer.clear();
    plugin->processBlock(buffer, midiBuffer);
}

juce::String PluginHost::getPluginPath() const {
    const juce::ScopedLock lock(pluginLock);
    return pluginPath;
}

juce::String PluginHost::getPluginName() const {
    const juce::ScopedLock lock(pluginLock);
    return pluginName;
}

double PluginHost::getSampleRate() const {
    const juce::ScopedLock lock(pluginLock);
    return currentSampleRate;
}

int PluginHost::getBufferSize() const {
    const juce::ScopedLock lock(pluginLock);
    return currentBufferSize;
}
