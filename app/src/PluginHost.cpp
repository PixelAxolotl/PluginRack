#include "PluginHost.h"

#include "RestrictedHost.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #endif

 namespace PluginHostDetail {

 // Crash-load tracking storage: the host children mark the file they are
 // inside so a crash can be blamed on it. Double-buffered so the crash handler
 // can read the path while a host child writes the next one: no allocation, no
 // lock, and never a half-written string. Written via PluginLoadGuard::begin,
 // read via currentLoadingPlugin[currentLoadingPluginBytes].
 constexpr std::size_t kMaxTrackedPath = 1024;
 char trackedPaths[2][kMaxTrackedPath] = {};
 std::atomic<int> trackedIndex{0};
 std::atomic<bool> tracking{false};

 // Marks the file a host child is inside (pairs with PluginLoadGuard::end).
 // Lives here (not in an anonymous namespace) so scanFileInProcess and
 // instantiateHostInstance can name it. The constructor calls
 // PluginLoadGuard::begin, so the bodies sit after that namespace below.
 struct ScopedPluginLoadMark {
     explicit ScopedPluginLoadMark(const juce::File& file);
     ~ScopedPluginLoadMark();

     juce::File path;
     bool armed = false;
 };

 // One VST3 format manager for the host processes: --scan-plugin and
 // --plugin-host instantiate there (the main app never maps a bundle).
 // Serialise the calls because the manager is not thread-safe.
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

// FNV-1a/64 over the file bytes: self-contained (no crypto dependency),
// good enough to tell "same binary" from "replaced binary" for the crash
// blocklist below.
juce::String fileContentHash(const juce::File& file, juce::int64& sizeOut) {
    sizeOut = -1;
    juce::FileInputStream stream(file);
    if (!stream.openedOk()) return {};
    sizeOut = stream.getTotalLength();
    std::uint64_t hash = 14695981039346656037ull;
    char buf[65536];
    while (!stream.isExhausted()) {
        const auto read = stream.read(buf, sizeof(buf));
        if (read <= 0) break;
        for (int i = 0; i < read; ++i) {
            hash ^= static_cast<unsigned char>(buf[i]);
            hash *= 1099511628211ull;
        }
    }
    return juce::String::toHexString((juce::int64) hash);
}

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

// PR-SEC-007: the old startsWith("\\\\") test missed mapped drives,
// \\?\UNC bypasses, WebDAV and junctions. Resolve the real path and test
// the volume, not the string.
bool isRemotePath(const juce::File& file) {
    const auto path = file.getFullPathName();
    // UNC strings are remote. An extended-local "\\?\C:\..." prefix is NOT
    // remote by itself — only "\\?\UNC\..." is.
    if (path.startsWith("\\\\?\\UNC\\")) return true;
    if (path.startsWith("\\\\") && !path.startsWith("\\\\?\\")) return true;
    std::wstring wide(path.toWideCharPointer());
    // Resolve junctions / mapped drives to the final volume path.
    HANDLE handle = ::CreateFileW(wide.c_str(), 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    std::wstring finalPath;
    if (handle != INVALID_HANDLE_VALUE) {
        wchar_t resolved[MAX_PATH * 4] = {};
        const DWORD length = ::GetFinalPathNameByHandleW(handle, resolved,
                                                         (DWORD) std::size(resolved),
                                                         VOLUME_NAME_DOS);
        ::CloseHandle(handle);
        if (length > 0 && length < std::size(resolved)) finalPath = resolved;
    }
    const std::wstring& probe = finalPath.empty() ? wide : finalPath;
    // Normalize the \\?\ extended-local prefix before testing: the resolver
    // returns "\\?\C:\..." for ordinary local files, which must not be
    // mistaken for UNC (both start with "\\").
    std::wstring norm = probe;
    if (norm.rfind(L"\\\\?\\UNC\\", 0) == 0) return true;
    if (norm.rfind(L"\\\\?\\", 0) == 0) norm = norm.substr(4);
    if (norm.rfind(L"\\??\\", 0) == 0) norm = norm.substr(4);
    if (norm.rfind(L"\\\\", 0) == 0) return true;
    // Volume test: anything not on a fixed local drive is refused.
    wchar_t root[MAX_PATH] = {};
    if (norm.size() >= 2 && norm[1] == L':') {
        root[0] = norm[0];
        root[1] = L':';
        root[2] = L'\\';
        root[3] = L'\0';
    } else if (!GetWindowsDirectoryW(root, MAX_PATH)) {
        return true;
    }
    const UINT driveType = ::GetDriveTypeW(root);
    if (driveType != DRIVE_FIXED) return true;
    // No blanket reparse-point refusal: OneDrive cloud files and ordinary
    // symlinks carry FILE_ATTRIBUTE_REPARSE_POINT while living on a local
    // fixed drive. A link that escapes to the network already resolves to a
    // UNC path above and is refused there.
    return false;
}

} // namespace PluginHostDetail

namespace PluginLoadGuard {

void begin(const juce::File& file) {
    const auto path = file.getFullPathName();
    const int next = 1 - PluginHostDetail::trackedIndex.load(std::memory_order_relaxed);
    PluginHostDetail::tracking.store(false, std::memory_order_release);
    const auto* utf8 = path.toRawUTF8();
    const auto length = std::min<std::size_t>(std::strlen(utf8),
                                              PluginHostDetail::kMaxTrackedPath - 1);
    std::memcpy(PluginHostDetail::trackedPaths[next], utf8, length);
    PluginHostDetail::trackedPaths[next][length] = '\0';
    PluginHostDetail::trackedIndex.store(next, std::memory_order_release);
    PluginHostDetail::tracking.store(true, std::memory_order_release);
}

void end() {
    PluginHostDetail::tracking.store(false, std::memory_order_release);
}

} // namespace PluginLoadGuard

PluginHostDetail::ScopedPluginLoadMark::ScopedPluginLoadMark(const juce::File& file)
    : path(file) {
    if (path.getFullPathName().isNotEmpty()) {
        PluginLoadGuard::begin(path);
        armed = true;
    }
}

PluginHostDetail::ScopedPluginLoadMark::~ScopedPluginLoadMark() {
    if (armed) PluginLoadGuard::end();
}

namespace PluginLoadGuard {

juce::String currentLoadingPlugin() {
    if (!PluginHostDetail::tracking.load(std::memory_order_acquire)) return {};
    const auto index = PluginHostDetail::trackedIndex.load(std::memory_order_acquire);
    return juce::String::fromUTF8(PluginHostDetail::trackedPaths[index]);
}

// PR-09: allocation-free snapshot for the crash handler. Copies the tracked
// path bytes into the caller's buffer; returns the length written.
std::size_t currentLoadingPluginBytes(char* dest, std::size_t destBytes) {
    if (dest == nullptr || destBytes == 0) return 0;
    if (!PluginHostDetail::tracking.load(std::memory_order_acquire)) {
        dest[0] = '\0';
        return 0;
    }
    const auto index = PluginHostDetail::trackedIndex.load(std::memory_order_acquire);
    std::size_t i = 0;
    for (; PluginHostDetail::trackedPaths[index][i] != '\0' && i + 1 < destBytes; ++i)
        dest[i] = PluginHostDetail::trackedPaths[index][i];
    dest[i] = '\0';
    return i;
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
    // PR-SEC-008: canonicalise + case-insensitive + hash-bound entries.
    // Format per line: "<lower canonical path>|<fnv1a64hex>|<size>|<mtime>".
    // A bare legacy path line still matches (compat), but a replaced binary
    // (hash/size/mtime differ) is treated as a new file and re-tried.
    const auto target = PluginPathUtils::canonicalPluginPath(file);
    juce::int64 size = -1;
    const juce::String hashHex = PluginHostDetail::fileContentHash(file, size);
    const juce::int64 mtime = file.getLastModificationTime().toMilliseconds();
    juce::StringArray lines;
    lines.addLines(list.loadFileAsString());
    for (const auto& line : lines) {
        const auto trimmed = line.trim();
        if (trimmed.isEmpty()) continue;
        if (!trimmed.containsChar('|')) {
            if (trimmed.toLowerCase() == target) return true;
            continue;
        }
        auto parts = juce::StringArray::fromTokens(trimmed, "|", {});
        if (parts.size() < 4) continue;
        if (parts[0].trim().toLowerCase() != target) continue;
        // Same path: block only when the binary still matches the one that
        // crashed (or when the entry predates hashes and has empty fields).
        const bool hashKnown = parts[1].trim().isNotEmpty();
        const bool sizeKnown = parts[2].trim().isNotEmpty();
        const bool timeKnown = parts[3].trim().isNotEmpty();
        if (!hashKnown && !sizeKnown && !timeKnown) return true;
        if (hashKnown && parts[1].trim().equalsIgnoreCase(hashHex)) return true;
        if (sizeKnown && parts[2].trim().getLargeIntValue() == size) return true;
        if (timeKnown && parts[3].trim().getLargeIntValue() == mtime) return true;
    }
    return false;
}

void block(const juce::File& file) {
    const auto canonical = PluginPathUtils::canonicalPluginPath(file);
    if (canonical.isEmpty() || isBlocked(file)) return;
    juce::int64 size = -1;
    const juce::String hashHex = PluginHostDetail::fileContentHash(file, size);
    const auto mtime = file.getLastModificationTime().toMilliseconds();
    auto list = blockedListFile();
    list.getParentDirectory().createDirectory();
    list.appendText(canonical + "|" + hashHex + "|" + juce::String(size) + "|"
                    + juce::String(mtime) + "\n");
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

namespace PluginPathUtils {

juce::String canonicalPluginPath(const juce::File& file) {
    const auto path = file.getFullPathName();
    if (path.isEmpty()) return {};
    // Resolve junctions / mapped drives to the final path so the blocklist
    // key cannot be dodged with an alternate spelling of the same binary.
    std::wstring wide(path.toWideCharPointer());
    HANDLE handle = ::CreateFileW(wide.c_str(), 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        wchar_t resolved[MAX_PATH * 4] = {};
        const DWORD length = ::GetFinalPathNameByHandleW(handle, resolved,
                                                         (DWORD) std::size(resolved),
                                                         VOLUME_NAME_DOS);
        ::CloseHandle(handle);
        if (length > 0 && length < std::size(resolved)) {
            return juce::String(resolved).toLowerCase();
        }
    }
    return path.toLowerCase();
}

} // namespace PluginPathUtils

PluginHost::PluginHost() = default;

PluginHost::~PluginHost() {
    const juce::ScopedLock lock(pluginLock);
    stopHostLocked();
}

bool PluginHost::hasPlugin() const {
    const juce::ScopedLock lock(pluginLock);
    return pluginPath.isNotEmpty();
}

bool PluginHost::isHostAlive() const {
    const juce::ScopedLock lock(pluginLock);
    return host != nullptr && hostReady && host->isAlive();
}

bool PluginHost::isHostDown() const {
    const juce::ScopedLock lock(pluginLock);
    return pluginPath.isNotEmpty() && (host == nullptr || !hostReady || !host->isAlive());
}

juce::String PluginHost::getStateAsBase64() const {
    // State lives in the host process; ask it over the control channel.
    // Empty when the host is down — never touch plugin code in-process.
    const juce::ScopedLock lock(pluginLock);
    if (host == nullptr || !hostReady || !host->isAlive()) return {};
    return host->sendControl("{\"getState\":1}");
}

int PluginHost::getLatencySamples() const {
    return cachedLatencySamples_.load(std::memory_order_relaxed);
}

void PluginHost::refreshLatencyCache() {
    const juce::ScopedTryLock lock(pluginLock);
    if (!lock.isLocked()) return;
    refreshLatencyCacheLocked();
}

void PluginHost::refreshLatencyCacheLocked() {
    // Latency is reported by the host over the control channel; while the
    // host is down the cached value stands (fail-closed keeps audio dry).
    if (host == nullptr || !hostReady || !host->isAlive()) return;
    const auto reply = host->sendControl("{\"getLatency\":1}");
    const int latency = reply.getIntValue();
    if (latency >= 0) cachedLatencySamples_.store(latency, std::memory_order_relaxed);
}

void PluginHost::editorBeingDeleted(juce::AudioProcessorEditor* ed) {
    juce::ignoreUnused(ed);
}

void PluginHost::clearEditorWindow(juce::DocumentWindow* window) {
    juce::ignoreUnused(window);
}

void PluginHost::closeEditorWindow(juce::DocumentWindow* window) {
    juce::ignoreUnused(window);
}

void PluginHost::showEditor() {
    // The editor lives in the host process; the dock bay embeds its native
    // window (see createEditorForDock). Floating in-process windows are gone.
    if (!hasPlugin()) return;
    if (!isHostAlive()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                              "PluginRack",
                                              "The sandboxed host is not running, so the "
                                              "plugin editor is unavailable.");
        return;
    }
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "PluginRack",
                                          "Open the plugin from the rack bay: the editor "
                                          "runs inside the sandboxed host.");
}

juce::AudioProcessorEditor* PluginHost::createEditorForDock() {
    // Out-of-process editors cannot be constructed in-process. The bay shows
    // the host-down badge until the host publishes its editor window handle
    // over the control channel (a follow-up to this change wires the native
    // reparenting). Returning null keeps every caller fail-closed.
    return nullptr;
}

void PluginHost::destroyDockEditor(juce::AudioProcessorEditor* editor) {
    // Parked: createEditorForDock never hands out editors while editors live
    // out-of-process, so there is nothing to destroy. Delete defensively in
    // case an old pointer reaches here.
    delete editor;
}

void PluginHost::closeFloatingEditor() {
    // Parked: no floating in-process windows exist while editors live
    // out-of-process.
}

juce::String PluginHost::loadPlugin(const juce::File& file, const juce::String& stateBase64,
                                    int requestedUniqueId) {
    // loadFromFile canonicalises to the bundle root (see its comment), so
    // adopt launches the host against the safe entry point even when the
    // scene names the inner binary.
    const auto loaded = loadFromFile(file, 0.0, 0, stateBase64, requestedUniqueId);
    if (loaded.error.isNotEmpty()) {
        return loaded.error;
    }
    adopt(loaded, stateBase64, requestedUniqueId);
    // Fail-closed: adopt reports a dead host via isHostDown; surface it as
    // an error so callers show the badge instead of silent dry audio.
    if (isHostDown()) {
        return "The sandboxed host did not start (" + file.getFileName()
               + "). The slot is silent until the host runs.";
    }
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

juce::Array<juce::PluginDescription> PluginHost::scanFile(const juce::File& file,
                                                          juce::String& error) {
    juce::Array<juce::PluginDescription> descriptions;

    if (PluginHostDetail::isRemotePath(file)) {
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
    const juce::StringArray arguments { executable.getFullPathName(),
                                        PluginHostDetail::kScanFlag,
                                        file.getFullPathName() };
    if (!executable.existsAsFile()
        || !child.start(arguments, juce::ChildProcess::wantStdOut)) {
        // Fail closed: scanning maps the plugin binary, and mapping it in
        // the main app would run it with the app's rights. Refuse instead.
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
            if (output.length() >= PluginHostDetail::kMaxScanOutputBytes) {
                outputCapped = true;
                break;
            }
        }
        if (juce::Time::getMillisecondCounterHiRes() - scanStart
            > PluginHostDetail::kScanTimeoutMs) {
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
        if (trimmed.startsWith(PluginHostDetail::kScanErrorPrefix)) {
            error = trimmed
                        .fromFirstOccurrenceOf(PluginHostDetail::kScanErrorPrefix, false, false)
                        .trim();
        } else if (trimmed.startsWith("HOSTERROR ")) {
            // Host child diagnostics (section/pipe failures): surface them so
            // the log names the failing step instead of going silent.
            juce::Logger::writeToLog("plugin: host child: " + trimmed);
        } else if (trimmed.startsWith(PluginHostDetail::kScanResultPrefix)) {
            juce::PluginDescription description;
            if (PluginHostDetail::describeFromJson(
                    trimmed.fromFirstOccurrenceOf(PluginHostDetail::kScanResultPrefix, false,
                                                 false),
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

    if (PluginHostDetail::isRemotePath(file)) {
        error = "Network plugin paths are not supported.";
        return descriptions;
    }
    if (!file.exists()) {
        error = "The selected VST3 does not exist.";
        return descriptions;
    }

    // Scanning maps the plugin binary, so a crash here is attributed to
    // this plugin. Only the --scan-plugin and --plugin-host children run this.
    const PluginHostDetail::ScopedPluginLoadMark loadMark(file);

    // Phase timings go to the run log: a plugin that stalls the UI is otherwise
    // only visible as a gap between two lines.
    const auto scanStart = juce::Time::getMillisecondCounterHiRes();
    const juce::ScopedLock lock(PluginHostDetail::sharedFormatLock());
    auto& manager = PluginHostDetail::sharedFormatManager();
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

PluginHost::HostInstance PluginHost::instantiateHostInstance(
    const juce::PluginDescription& description, const juce::File& file, double sampleRate,
    int bufferSize, const juce::String& stateBase64) {
    HostInstance loaded;
    if (PluginHostDetail::isRemotePath(file)) {
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
    const juce::ScopedLock lock(PluginHostDetail::sharedFormatLock());
    auto& manager = PluginHostDetail::sharedFormatManager();
    if (manager.getNumFormats() == 0) {
        loaded.error = "No VST3 plugin format is available.";
        return loaded;
    }

    juce::String errorMessage;
    const PluginHostDetail::ScopedPluginLoadMark loadMark(file);
    std::cout << "HOSTDIAG instantiating " << file.getFullPathName().toStdString() << "\n";
    std::cout.flush();
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
        if (stateBase64.length() > PluginHostDetail::kMaxPluginStateBase64Chars) {
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
    std::cout << "HOSTDIAG instantiated ok\n";
    std::cout.flush();
    return loaded;
}

PluginHost::LoadedPlugin PluginHost::loadFromFile(const juce::File& file, double sampleRate,
                                                  int bufferSize, const juce::String& stateBase64,
                                                  int pluginUniqueId) {
    juce::ignoreUnused(sampleRate, bufferSize, stateBase64);
    LoadedPlugin loaded;
    // Canonicalise to the bundle root first: the scene may name the inner
    // binary (Contents\x86_64-win\*.vst3) while the scan reports the bundle
    // root (*.vst3). Instantiating the inner binary directly crashes the
    // host ~13 s after launch; the bundle root is the only safe entry
    // point. Layout is <Name>.vst3\Contents\x86_64-win\<Name>.vst3, so climb
    // to the first *.vst3 ancestor (the inner binary's own parent chain
    // passes through Contents/arch folders, never a *.vst3 folder). All
    // checks below (remote, exists, blocked, scan) run against the
    // canonical path.
    auto bundleFile = file;
    if (file.getFullPathName().containsIgnoreCase("Contents\\x86_64-win")) {
        auto root = file;
        for (;;) {
            const auto up = root.getParentDirectory();
            const auto name = up.getFileName();
            if (name.endsWithIgnoreCase(".vst3") && up != root) {
                root = up;
                break;
            }
            if (up == root || up.getFullPathName().isEmpty()) break;
            root = up;
        }
        if (root.getFileName().endsWithIgnoreCase(".vst3")) bundleFile = root;
    }
    if (bundleFile.getFullPathName() != file.getFullPathName()) {
        juce::Logger::writeToLog("plugin: resolved bundle root " + bundleFile.getFullPathName()
                                 + " from " + file.getFullPathName());
    }
    loaded.path = bundleFile.getFullPathName();
    if (PluginHostDetail::isRemotePath(bundleFile)) {
        loaded.error = "Network plugin paths are not supported.";
        return loaded;
    }
    if (!bundleFile.exists()) {
        loaded.error = "The selected VST3 does not exist.";
        return loaded;
    }
    if (PluginLoadGuard::isBlocked(bundleFile)) {
        loaded.error = "This plugin crashed PluginRack while it was loading, so it is blocked. "
                       "Delete " + PluginLoadGuard::blockedListFile().getFullPathName()
                       + " to let it load again.";
        return loaded;
    }
    juce::String error;
    const auto descriptions = scanFile(bundleFile, error);
    if (descriptions.isEmpty()) {
        loaded.error = error.isNotEmpty() ? error : "No VST3 plugin was found in the selected file.";
        return loaded;
    }
    // A bundle can hold several classes; the scene records which one this slot
    // was created from. An unknown or absent id keeps the old first-class
    // behaviour instead of failing the load. Metadata only — the instance is
    // created inside --plugin-host, never here.
    const juce::PluginDescription* chosen = &descriptions.getReference(0);
    for (const auto& description : descriptions) {
        if (pluginUniqueId != 0 && description.uniqueId == pluginUniqueId) {
            chosen = &description;
            break;
        }
    }
    loaded.name = chosen->name.isNotEmpty() ? chosen->name : bundleFile.getFileNameWithoutExtension();
    loaded.uniqueId = chosen->uniqueId;
    return loaded;
}

void PluginHost::startHostLocked(const juce::File& file, int uniqueId,
                                 const juce::String& stateBase64, juce::String& error) {
    stopHostLocked();
    host = std::make_unique<RestrictedHost>();
    hostReady = false;
    if (!host->launch(file, uniqueId, stateBase64, error)) {
        host.reset();
        return;
    }
    // launch() already delivered the state over the control pipe; a second
    // send would duplicate it.
    hostReady = host->isAlive();
    if (hostReady) everReady = true;
    else {
        error = "The sandboxed plugin host is not responding. No plugin was loaded.";
    }
}

bool PluginHost::wasEverReady() const {
    const juce::ScopedLock lock(pluginLock);
    return everReady;
}

bool PluginHost::hasHost() const {
    const juce::ScopedLock lock(pluginLock);
    return host != nullptr;
}

unsigned long PluginHost::lastHostExitCode() const {
    const juce::ScopedLock lock(pluginLock);
    if (host == nullptr) return 0;
    return host->lastChildExitCode();
}

unsigned long long PluginHost::requestEditorWindow(int& widthOut, int& heightOut) {
    const juce::ScopedLock lock(pluginLock);
    widthOut = 0;
    heightOut = 0;
    if (host == nullptr || !hostReady || !host->isAlive()) return 0;
    return host->requestEditorWindow(widthOut, heightOut);
}

juce::String PluginHost::lastEditorFailReason() const {
    const juce::ScopedLock lock(pluginLock);
    if (host == nullptr) return "host-gone";
    return host->lastEditorFailReason();
}

void PluginHost::sendEditorScale(float scale) {
    const juce::ScopedLock lock(pluginLock);
    if (host == nullptr) return;
    host->sendEditorScale(scale);
}

bool PluginHost::requestEditorSize(int& widthOut, int& heightOut) {
    const juce::ScopedLock lock(pluginLock);
    widthOut = 0;
    heightOut = 0;
    if (host == nullptr || !hostReady || !host->isAlive()) return false;
    return host->requestEditorSize(widthOut, heightOut);
}

void PluginHost::releaseEditorWindow() {
    const juce::ScopedLock lock(pluginLock);
    if (host == nullptr) return;
    host->releaseEditorWindow();
}

void PluginHost::stopHostLocked() {
    if (host != nullptr) {
        // Blame the stop: adopt() calls this before every fresh launch
        // (expected), but anything else killing a live host is a bug.
        // The watchdog's "died after launch" line cannot tell the two
        // apart without this.
        juce::Logger::writeToLog("plugin: host stop requested for "
                                 + (pluginName.isNotEmpty() ? pluginName : pluginPath)
                                 + (host->isAlive() ? " (was alive)" : " (already dead)"));
        host->killAndReap();
        host.reset();
    }
    hostReady = false;
    hostSequence = 0;
}

void PluginHost::adopt(const LoadedPlugin& loaded, const juce::String& stateBase64,
                       int requestedUniqueId) {
    if (loaded.error.isNotEmpty() || loaded.path.isEmpty()) return;
    const juce::ScopedLock lock(pluginLock);
    stopHostLocked();
    pluginPath = loaded.path;
    pluginName = loaded.name.isNotEmpty() ? loaded.name
                                          : juce::File(loaded.path).getFileNameWithoutExtension();
    pluginUniqueId = requestedUniqueId != 0 ? requestedUniqueId : loaded.uniqueId;
    // Out-of-process-only: the instance lives in the sandboxed host.
    // Fail-closed: a dead host leaves the slot silent with a badge.
    juce::String hostError;
    startHostLocked(juce::File(loaded.path), pluginUniqueId, stateBase64, hostError);
    if (host != nullptr && !hostError.isEmpty()) {
        juce::Logger::writeToLog("plugin: sandboxed host: " + hostError);
    }
    prepared = hostReady;
    if (hostReady) refreshLatencyCacheLocked();
    juce::Logger::writeToLog("plugin: prepare "
                             + (hostReady ? juce::String(" (sandboxed host)")
                                          : juce::String(" (host unavailable, slot silent)")));
}

void PluginHost::prepareToPlay(double sampleRate, int samplesPerBlock) {
    const juce::ScopedLock lock(pluginLock);
    currentSampleRate = sampleRate;
    currentBufferSize = samplesPerBlock;
    // Format only: the host prepares its own instance on first block.
    prepared = host != nullptr && hostReady && host->isAlive();
}

void PluginHost::releaseResources() {
    const juce::ScopedLock lock(pluginLock);
    prepared = false;
}

void PluginHost::processBlock(juce::AudioBuffer<float>& buffer, bool bypassed) {
    if (bypassed) {
        return;
    }

    const juce::ScopedTryLock lock(pluginLock);
    if (!lock.isLocked() || pluginPath.isEmpty()) {
        return;
    }

    // Seamless host hop: the live buffer is never mutated until the host
    // has acked a full block. Submit copies the range into hostScratch so
    // a lagging host cannot mix two blocks (the zipper), and fetch writes
    // the host's answer back into hostScratch; only a clean ack commits it
    // into the live buffer. Fail-closed: a dead host passes dry audio and
    // the strip badge shows "host down" — never an in-process render.
    if (host != nullptr && hostReady && host->isAlive()) {
        const int frames = buffer.getNumSamples();
        const int needed = juce::jmax(frames, 1);
        if (hostScratchCapacity < needed) {
            hostScratch.setSize(2, needed, false, false, true);
            hostScratchCapacity = needed;
        }
        hostScratch.copyFrom(0, 0, buffer, 0, 0, frames);
        hostScratch.copyFrom(1, 0, buffer, 1, 0, frames);
        // Non-blocking submit: if the host is still producing the previous
        // block, pass through dry for one block rather than corrupt the
        // stream. The next callback retries the host when it has caught up.
        if (host->submitAudioBlock(hostScratch, currentSampleRate, currentBufferSize)) {
            ++hostSequence;
            double hostRate = 0.0;
            int hostBufferSize = 0;
            if (host->fetchAudioBlock(hostScratch, hostSequence, hostRate, hostBufferSize)) {
                // Commit only when the header still matches the live format.
                if (hostRate <= 0.0 || juce::approximatelyEqual(hostRate, currentSampleRate)) {
                    buffer.copyFrom(0, 0, hostScratch, 0, 0, frames);
                    buffer.copyFrom(1, 0, hostScratch, 1, 0, frames);
                }
                return;
            }
            // Host did not answer within the budget: leave the dry buffer.
            hostReady = host->isAlive();
            return;
        }
        hostReady = host->isAlive();
    } else {
        hostReady = false;
    }
    // Fail-closed: dry audio, no in-process fallback.
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
