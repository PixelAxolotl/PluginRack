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

// One VST3 format manager for the whole app: the scene loader instantiates on
// the message thread while the Add-VST path instantiates on a worker, and the
// manager caches the opened bundle per format instance. Serialise the calls
// because it is not thread-safe.
juce::AudioPluginFormatManager& sharedFormatManager() {
    // Deliberately process-lifetime: the format manager caches opened VST3
    // bundles, so it must outlive every host and worker that uses it.
    static auto* manager = [] {
        auto* registered = new juce::AudioPluginFormatManager();
        registered->addFormat(std::make_unique<juce::VST3PluginFormat>());
        return registered;
    }();
    return *manager;
}

juce::CriticalSection& sharedFormatLock() {
    static juce::CriticalSection lock;
    return lock;
}
} // namespace

PluginHost::PluginHost() = default;

PluginHost::~PluginHost() {
    delete editorWindow;
    editorWindow = nullptr;
    const juce::ScopedLock lock(pluginLock);
    if (plugin != nullptr) {
        plugin->releaseResources();
    }
}

bool PluginHost::hasPlugin() const {
    const juce::ScopedLock lock(pluginLock);
    return plugin != nullptr;
}

juce::String PluginHost::getStateAsBase64() const {
    const juce::ScopedLock lock(pluginLock);
    if (plugin == nullptr) return {};
    juce::MemoryBlock state;
    plugin->getStateInformation(state);
    return state.getSize() > 0 ? state.toBase64Encoding() : juce::String{};
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

    {
        // Single-editor rule: the processor allows only one active editor.
        // The rack dock may already own it — never create a second owner
        // (reparenting the docked editor into a floating window leaves the
        // dock with a dangling pointer and double-deletes on remove).
        const juce::ScopedLock lock(pluginLock);
        if (plugin != nullptr && plugin->getActiveEditor() != nullptr) return;
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

juce::AudioProcessorEditor* PluginHost::createEditorForDock() {
    closeFloatingEditor();
    const juce::ScopedLock lock(pluginLock);
    if (plugin != nullptr && plugin->hasEditor()) {
        return plugin->createEditorAndMakeActive();
    }
    return nullptr;
}

void PluginHost::destroyDockEditor(juce::AudioProcessorEditor* editor) {
    if (editor == nullptr) return;
    // The editor may have been reparented into a floating window, which
    // owns it now: don't delete it out from under the window, just let the
    // window keep sole ownership.
    if (editor->getParentComponent() != nullptr) return;
    editorBeingDeleted(editor);
    delete editor;
}

void PluginHost::closeFloatingEditor() {
    if (editorWindow == nullptr) return;
    closeEditorWindow(editorWindow);
}

juce::String PluginHost::loadPlugin(const juce::File& file, const juce::String& stateBase64) {
    double sampleRate = 0.0;
    int bufferSize = 0;
    {
        const juce::ScopedLock lock(pluginLock);
        sampleRate = currentSampleRate;
        bufferSize = currentBufferSize;
    }

    const auto loaded = loadFromFile(file, sampleRate, bufferSize, stateBase64);
    if (loaded.error.isNotEmpty()) {
        return loaded.error;
    }
    adopt(loaded);
    return {};
}

PluginHost::LoadedPlugin PluginHost::loadFromFile(const juce::File& file, double sampleRate,
                                                  int bufferSize, const juce::String& stateBase64) {
    LoadedPlugin loaded;
    loaded.path = file.getFullPathName();
    if (!file.exists()) {
        loaded.error = "The selected VST3 does not exist.";
        return loaded;
    }

    const juce::ScopedLock lock(sharedFormatLock());
    auto& manager = sharedFormatManager();
    if (manager.getNumFormats() == 0) {
        loaded.error = "No VST3 plugin format is available.";
        return loaded;
    }

    juce::OwnedArray<juce::PluginDescription> descriptions;
    manager.getFormat(0)->findAllTypesForFile(descriptions, loaded.path);
    if (descriptions.isEmpty()) {
        loaded.error = "No VST3 plugin was found in the selected file.";
        return loaded;
    }

    juce::String errorMessage;
    auto candidate = manager.createPluginInstance(*descriptions[0], sampleRate, bufferSize,
                                                  errorMessage);
    if (candidate == nullptr) {
        loaded.error = errorMessage.isNotEmpty() ? errorMessage
                                                 : "The VST3 could not be instantiated.";
        return loaded;
    }

    // The host supplies one stereo audio block. Auxiliary and sidechain
    // buses must be disabled explicitly; leaving them active makes some VST3
    // processors interpret the two-channel block with the wrong bus offsets.
    if (!candidate->disableNonMainBuses()) {
        loaded.error = "The selected VST3 could not disable its auxiliary buses.";
        return loaded;
    }

    auto layout = candidate->getBusesLayout();
    if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty()) {
        loaded.error = "The selected plugin is not an audio effect with input and output buses.";
        return loaded;
    }

    layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (!candidate->setBusesLayout(layout)) {
        loaded.error = "The selected plugin does not support stereo input and output.";
        return loaded;
    }

    candidate->setRateAndBufferSizeDetails(sampleRate, bufferSize);
    // Apply saved state before activation: that is the host flow plugins see
    // when a project is reopened, and it avoids a default-parameters render
    // pass slipping through before the restore takes effect.
    if (stateBase64.isNotEmpty()) {
        juce::MemoryBlock state;
        if (state.fromBase64Encoding(stateBase64)) {
            candidate->setStateInformation(state.getData(), (int) state.getSize());
        }
    }

    loaded.name = candidate->getName();
    loaded.instance = std::move(candidate);
    return loaded;
}

void PluginHost::adopt(const LoadedPlugin& loaded) {
    if (loaded.instance == nullptr) return;
    const juce::ScopedLock lock(pluginLock);
    if (plugin != nullptr) {
        plugin->releaseResources();
    }
    pluginPath = loaded.path;
    pluginName = loaded.name;
    plugin = loaded.instance;
    // prepareToPlay runs here, on the message thread: some plugins synchronise
    // with it (see AudioEngine's device-open comment).
    plugin->prepareToPlay(currentSampleRate, currentBufferSize);
    prepared = true;
    cachedLatencySamples_.store(plugin->getLatencySamples(), std::memory_order_relaxed);
}

void PluginHost::prepareToPlay(double sampleRate, int samplesPerBlock) {
    const juce::ScopedLock lock(pluginLock);
    const bool unchanged = prepared && currentSampleRate == sampleRate
        && currentBufferSize == samplesPerBlock;
    currentSampleRate = sampleRate;
    currentBufferSize = samplesPerBlock;
    if (plugin != nullptr && !unchanged) {
        if (prepared) {
            plugin->releaseResources();
        }
        plugin->setRateAndBufferSizeDetails(sampleRate, samplesPerBlock);
        plugin->prepareToPlay(sampleRate, samplesPerBlock);
        prepared = true;
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
