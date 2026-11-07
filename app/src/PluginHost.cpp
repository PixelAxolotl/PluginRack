#include "PluginHost.h"

#include "RestrictedHost.h"

#include <algorithm>
#include <climits>
#include <cstring>

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

// Double-buffered so the crash handler can read the path while a worker writes
// the next one: no allocation, no lock, and never a half-written string.
constexpr std::size_t kMaxTrackedPath = 1024;
char trackedPaths[2][kMaxTrackedPath] = {};
std::atomic<int> trackedIndex{0};
std::atomic<bool> tracking{false};

struct ScopedPluginLoadMark {
    explicit ScopedPluginLoadMark(const juce::File& file) : path(file) {
        if (path.getFullPathName().isNotEmpty()) {
            PluginLoadGuard::begin(path);
            armed = true;
        }
    }
    ~ScopedPluginLoadMark() {
        if (armed) PluginLoadGuard::end();
    }

    juce::File path;
    bool armed = false;
};
} // namespace

namespace PluginLoadGuard {

void begin(const juce::File& file) {
    const auto path = file.getFullPathName();
    const int next = 1 - trackedIndex.load(std::memory_order_relaxed);
    tracking.store(false, std::memory_order_release);
    const auto* utf8 = path.toRawUTF8();
    const auto length = std::min<std::size_t>(std::strlen(utf8), kMaxTrackedPath - 1);
    std::memcpy(trackedPaths[next], utf8, length);
    trackedPaths[next][length] = '\0';
    trackedIndex.store(next, std::memory_order_release);
    tracking.store(true, std::memory_order_release);
}

void end() {
    tracking.store(false, std::memory_order_release);
}

juce::String currentLoadingPlugin() {
    if (!tracking.load(std::memory_order_acquire)) return {};
    const auto index = trackedIndex.load(std::memory_order_acquire);
    return juce::String::fromUTF8(trackedPaths[index]);
}

juce::File blockedListFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("PluginRack")
        .getChildFile("plugins")
        .getChildFile("blocked-plugin-loads.txt");
}

bool isBlocked(const juce::File& file) {
    const auto list = blockedListFile();
    if (!list.existsAsFile()) return false;
    const auto target = file.getFullPathName();
    juce::StringArray lines;
    lines.addLines(list.loadFileAsString());
    for (const auto& line : lines) {
        if (line.trim() == target) return true;
    }
    return false;
}

void block(const juce::File& file) {
    const auto path = file.getFullPathName();
    if (path.isEmpty() || isBlocked(file)) return;
    auto list = blockedListFile();
    list.getParentDirectory().createDirectory();
    list.appendText(path + "\n");
}

int clearBlocked() {
    auto list = blockedListFile();
    if (!list.existsAsFile()) return 0;
    juce::StringArray lines;
    lines.addLines(list.loadFileAsString());
    lines.removeEmptyStrings();
    const int count = lines.size();
    list.deleteFile();
    return count;
}

} // namespace PluginLoadGuard

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

juce::AudioPluginInstance* PluginHost::getInstance() const {
    const juce::ScopedLock lock(pluginLock);
    return plugin.get();
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
        const auto start = juce::Time::getMillisecondCounterHiRes();
        auto* editor = plugin->createEditorAndMakeActive();
        juce::Logger::writeToLog("plugin: editor "
                                 + juce::String(
                                       juce::Time::getMillisecondCounterHiRes() - start, 1)
                                 + " ms");
        return editor;
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

juce::String PluginHost::loadPlugin(const juce::File& file, const juce::String& stateBase64,
                                    int pluginUniqueId) {
    double sampleRate = 0.0;
    int bufferSize = 0;
    {
        const juce::ScopedLock lock(pluginLock);
        sampleRate = currentSampleRate;
        bufferSize = currentBufferSize;
    }

    const auto loaded = loadFromFile(file, sampleRate, bufferSize, stateBase64, pluginUniqueId);
    if (loaded.error.isNotEmpty()) {
        return loaded.error;
    }
    adopt(loaded);
    return {};
}

juce::String PluginHost::describeAsJson(const juce::PluginDescription& description) {
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    object->setProperty("name", description.name);
    object->setProperty("descriptiveName", description.descriptiveName);
    object->setProperty("format", description.pluginFormatName);
    object->setProperty("category", description.category);
    object->setProperty("manufacturer", description.manufacturerName);
    object->setProperty("version", description.version);
    object->setProperty("file", description.fileOrIdentifier);
    object->setProperty("uniqueId", (juce::int64) description.uniqueId);
    object->setProperty("isInstrument", description.isInstrument);
    object->setProperty("numInputs", description.numInputChannels);
    object->setProperty("numOutputs", description.numOutputChannels);
    object->setProperty("hasARA", description.hasARAExtension);
    return juce::JSON::toString(juce::var(object.get()), true);
}

namespace {

bool describeFromJson(const juce::String& text, juce::PluginDescription& description) {
    const auto parsed = juce::JSON::parse(text);
    if (!parsed.isObject()) {
        return false;
    }
    description = juce::PluginDescription{};
    description.name = parsed.getProperty("name", {}).toString();
    description.descriptiveName = parsed.getProperty("descriptiveName", {}).toString();
    description.pluginFormatName = parsed.getProperty("format", {}).toString();
    description.category = parsed.getProperty("category", {}).toString();
    description.manufacturerName = parsed.getProperty("manufacturer", {}).toString();
    description.version = parsed.getProperty("version", {}).toString();
    description.fileOrIdentifier = parsed.getProperty("file", {}).toString();
    description.uniqueId = (int) (juce::int64) parsed.getProperty("uniqueId", 0);
    description.isInstrument = (bool) parsed.getProperty("isInstrument", false);
    description.numInputChannels = (int) parsed.getProperty("numInputs", 0);
    description.numOutputChannels = (int) parsed.getProperty("numOutputs", 0);
    description.hasARAExtension = (bool) parsed.getProperty("hasARA", false);
    return description.name.isNotEmpty() && description.fileOrIdentifier.isNotEmpty();
}

constexpr const char* kScanFlag = "--scan-plugin";
constexpr const char* kScanResultPrefix = "PLUGINDESC ";
constexpr const char* kScanErrorPrefix = "PLUGINSCANERROR ";
constexpr int kMaxPluginStateBase64Chars = 1 * 1024 * 1024;

// Scanner bounds (PR-02): a hung plugin must not hold the scan worker
// forever, and a chatty one must not grow the captured output without end.
// 30 s is far above the slowest known good scan (~3 s for big libraries).
constexpr int kScanTimeoutMs = 30000;
constexpr int kMaxScanOutputBytes = 1 * 1024 * 1024;

bool isRemotePath(const juce::File& file) {
    return file.getFullPathName().startsWith("\\\\");
}

} // namespace

juce::Array<juce::PluginDescription> PluginHost::scanFile(const juce::File& file,
                                                          juce::String& error) {
    juce::Array<juce::PluginDescription> descriptions;

    if (isRemotePath(file)) {
        error = "Network plugin paths are not supported.";
        return descriptions;
    }
    if (!file.exists()) {
        error = "The selected VST3 does not exist.";
        return descriptions;
    }

    if (PluginLoadGuard::isBlocked(file)) {
        error = "This plugin crashed PluginRack while it was loading, so it is blocked. "
                "Delete " + PluginLoadGuard::blockedListFile().getFullPathName()
                + " to let it load again.";
        juce::Logger::writeToLog("plugin: blocked " + file.getFileName());
        return descriptions;
    }

    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::ChildProcess child;
    const juce::StringArray arguments { executable.getFullPathName(), kScanFlag,
                                        file.getFullPathName() };
    if (!executable.existsAsFile()
        || !child.start(arguments, juce::ChildProcess::wantStdOut)) {
        // Fail closed: scanning runs plugin code, and running it in the main
        // app would give a plugin the app's privileges. Refuse instead.
        juce::Logger::writeToLog("plugin: scan process unavailable, refusing in-process scan");
        error = "Plugin scanning is unavailable right now. The plugin was not loaded.";
        return descriptions;
    }

    const auto scanStart = juce::Time::getMillisecondCounterHiRes();
    juce::String output;
    bool scanTimedOut = false;
    bool outputCapped = false;
    while (child.isRunning()) {
        char chunk[4096];
        const int read = child.readProcessOutput(chunk, sizeof(chunk));
        if (read > 0) {
            output += juce::String::fromUTF8(chunk, read);
            if (output.length() >= kMaxScanOutputBytes) {
                outputCapped = true;
                break;
            }
        }
        if (juce::Time::getMillisecondCounterHiRes() - scanStart > kScanTimeoutMs) {
            scanTimedOut = true;
            break;
        }
        juce::Thread::sleep(25);
    }
    if (scanTimedOut || outputCapped || child.isRunning()) {
        // Stuck or flooding child: kill it and reap it so the worker frees up.
        child.kill();
        child.waitForProcessToFinish(5000);
        juce::Logger::writeToLog("plugin: scan aborted "
                                 + juce::String(scanTimedOut ? "timeout" : "output limit"));
        error = scanTimedOut ? "The plugin took too long to scan. It was not loaded."
                             : "The plugin produced too much scan output. It was not loaded.";
        return descriptions;
    }
    output += child.readAllProcessOutput();
    juce::Logger::writeToLog("plugin: scan "
                             + juce::String(
                                   juce::Time::getMillisecondCounterHiRes() - scanStart, 1)
                             + " ms");

    juce::StringArray lines;
    lines.addLines(output);
    for (const auto& line : lines) {
        const auto trimmed = line.trim();
        if (trimmed.startsWith(kScanErrorPrefix)) {
            error = trimmed.fromFirstOccurrenceOf(kScanErrorPrefix, false, false).trim();
        } else if (trimmed.startsWith(kScanResultPrefix)) {
            juce::PluginDescription description;
            if (describeFromJson(trimmed.fromFirstOccurrenceOf(kScanResultPrefix, false, false),
                                 description)) {
                descriptions.add(description);
            }
        }
    }

    if (descriptions.isEmpty() && error.isEmpty()) {
        error = child.getExitCode() == 0
                    ? "No VST3 plugin was found in the selected file."
                    : "The plugin failed while being scanned. It was added to the blocked list.";
    }
    return descriptions;
}

juce::Array<juce::PluginDescription> PluginHost::scanFileInProcess(const juce::File& file,
                                                                   juce::String& error) {
    juce::Array<juce::PluginDescription> descriptions;

    if (isRemotePath(file)) {
        error = "Network plugin paths are not supported.";
        return descriptions;
    }
    if (!file.exists()) {
        error = "The selected VST3 does not exist.";
        return descriptions;
    }

    // Scanning runs plugin code, so a crash here is attributed to this plugin.
    const ScopedPluginLoadMark loadMark(file);

    // Phase timings go to the run log: a plugin that stalls the UI is otherwise
    // only visible as a gap between two lines.
    const auto scanStart = juce::Time::getMillisecondCounterHiRes();
    const juce::ScopedLock lock(sharedFormatLock());
    auto& manager = sharedFormatManager();
    if (manager.getNumFormats() == 0) {
        error = "No VST3 plugin format is available.";
        return descriptions;
    }

    juce::OwnedArray<juce::PluginDescription> found;
    manager.getFormat(0)->findAllTypesForFile(found, file.getFullPathName());
    juce::Logger::writeToLog("plugin: scan "
                             + juce::String(
                                   juce::Time::getMillisecondCounterHiRes() - scanStart, 1)
                             + " ms");
    if (found.isEmpty()) {
        error = "No VST3 plugin was found in the selected file.";
        return descriptions;
    }

    for (const auto* description : found) {
        descriptions.add(*description);
    }
    return descriptions;
}

PluginHost::LoadedPlugin PluginHost::instantiate(const juce::PluginDescription& description,
                                                 const juce::File& file, double sampleRate,
                                                 int bufferSize,
                                                 const juce::String& stateBase64) {
    LoadedPlugin loaded;
    loaded.path = file.getFullPathName();
    if (isRemotePath(file)) {
        loaded.error = "Network plugin paths are not supported.";
        return loaded;
    }
    if (!file.exists()) {
        loaded.error = "The selected VST3 does not exist.";
        return loaded;
    }

    if (PluginLoadGuard::isBlocked(file)) {
        loaded.error = "This plugin crashed PluginRack while it was loading, so it is blocked. "
                       "Delete " + PluginLoadGuard::blockedListFile().getFullPathName()
                       + " to let it load again.";
        juce::Logger::writeToLog("plugin: blocked " + file.getFileName());
        return loaded;
    }

    const auto createStart = juce::Time::getMillisecondCounterHiRes();
    const juce::ScopedLock lock(sharedFormatLock());
    auto& manager = sharedFormatManager();
    if (manager.getNumFormats() == 0) {
        loaded.error = "No VST3 plugin format is available.";
        return loaded;
    }

    juce::String errorMessage;
    const ScopedPluginLoadMark loadMark(file);
    auto candidate = manager.createPluginInstance(description, sampleRate, bufferSize,
                                                  errorMessage);
    juce::Logger::writeToLog("plugin: instantiate "
                             + juce::String(
                                   juce::Time::getMillisecondCounterHiRes() - createStart, 1)
                             + " ms");
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
        if (stateBase64.length() > kMaxPluginStateBase64Chars) {
            loaded.error = "The saved plugin state is too large.";
            return loaded;
        }
        juce::MemoryBlock state;
        if (state.fromBase64Encoding(stateBase64)
            && state.getSize() <= static_cast<std::size_t>(INT_MAX)) {
            candidate->setStateInformation(state.getData(), (int) state.getSize());
        } else if (state.getSize() > static_cast<std::size_t>(INT_MAX)) {
            loaded.error = "The saved plugin state is too large.";
            return loaded;
        }
    }

    loaded.name = candidate->getName();
    loaded.instance = std::move(candidate);
    juce::Logger::writeToLog("plugin: configure "
                             + juce::String(
                                   juce::Time::getMillisecondCounterHiRes() - createStart, 1)
                             + " ms");
    return loaded;
}

PluginHost::LoadedPlugin PluginHost::loadFromFile(const juce::File& file, double sampleRate,
                                                  int bufferSize, const juce::String& stateBase64,
                                                  int pluginUniqueId) {
    juce::String error;
    const auto descriptions = scanFile(file, error);
    if (descriptions.isEmpty()) {
        LoadedPlugin loaded;
        loaded.path = file.getFullPathName();
        loaded.error = error.isNotEmpty() ? error : "No VST3 plugin was found in the selected file.";
        return loaded;
    }
    // A bundle can hold several classes; the scene records which one this slot
    // was created from. An unknown or absent id keeps the old first-class
    // behaviour instead of failing the load.
    for (const auto& description : descriptions) {
        if (pluginUniqueId != 0 && description.uniqueId == pluginUniqueId) {
            return instantiate(description, file, sampleRate, bufferSize, stateBase64);
        }
    }
    return instantiate(descriptions.getReference(0), file, sampleRate, bufferSize, stateBase64);
}

void PluginHost::startHostLocked(const juce::File& file, juce::String& error) {
    stopHostLocked();
    host = std::make_unique<RestrictedHost>();
    hostReady = false;
    if (!host->launch(file, error)) {
        host.reset();
        return;
    }
    hostReady = host->isAlive();
    if (!hostReady) {
        error = "The restricted plugin host is not responding. No plugin was loaded.";
    }
}

void PluginHost::stopHostLocked() {
    if (host != nullptr) {
        host->killAndReap();
        host.reset();
    }
    hostReady = false;
    hostSequence = 0;
}

void PluginHost::adopt(const LoadedPlugin& loaded) {
    if (loaded.instance == nullptr) return;
    // prepareToPlay runs plugin code, so keep the load guard pointing at it.
    const ScopedPluginLoadMark loadMark(juce::File(loaded.path));
    const juce::ScopedLock lock(pluginLock);
    if (plugin != nullptr) {
        plugin->releaseResources();
    }
    pluginPath = loaded.path;
    pluginName = loaded.name;
    plugin = loaded.instance;
    // The plugin runs in the restricted host process (PR-01). The in-process
    // instance in `plugin` is a metadata-only handle; audio goes through the
    // host's shared-memory block.
    juce::String hostError;
    startHostLocked(juce::File(loaded.path), hostError);
    if (host != nullptr && !hostError.isEmpty()) {
        juce::Logger::writeToLog("plugin: restricted host: " + hostError);
    }
    const auto prepareStart = juce::Time::getMillisecondCounterHiRes();
    plugin->prepareToPlay(currentSampleRate, currentBufferSize);
    prepared = true;
    cachedLatencySamples_.store(plugin->getLatencySamples(), std::memory_order_relaxed);
    juce::Logger::writeToLog("plugin: prepare "
                             + juce::String(
                                   juce::Time::getMillisecondCounterHiRes() - prepareStart, 1)
                             + " ms"
                             + (hostReady ? " (restricted host)" : " (host unavailable)"));
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

    // Audio goes through the restricted host's shared-memory block. The
    // in-process instance is never asked to render: that would run plugin
    // code in this app. If the host is down the block passes through dry.
    if (host != nullptr && hostReady && host->isAlive()) {
        if (host->submitAudioBlock(buffer, currentSampleRate, currentBufferSize)) {
            ++hostSequence;
            host->fetchAudioBlock(buffer, hostSequence);
            return;
        }
    }

    midiBuffer.clear();
    // Fallback: host unavailable room tone. Do NOT call plugin->processBlock
    // here — that is plugin code in this process (PR-01). Render silence so
    // the slot is audible as "host down" in the meters instead.
    buffer.clear(0, buffer.getNumSamples());
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
