#include "PluginLibrary.h"

#include "PluginHost.h"

#include <algorithm>

namespace {

// VST3 bundles are folders ending in .vst3. VST2 (.dll) joins this wildcard
// when that format is compiled in (JUCE_PLUGINHOST_VST).
constexpr const char* kBundleWildcard = "*.vst3";
constexpr int kMaxScanDepth = 8;
constexpr int kMaxScanBundles = 4096;

bool caseInsensitiveLess(const juce::String& a, const juce::String& b) {
    return a.compareIgnoreCase(b) < 0;
}

bool collectBundles(const juce::File& folder, juce::Array<juce::File>& bundles, int depth) {
    if (!folder.isDirectory() || depth > kMaxScanDepth) return true;
    for (const auto& entry : juce::RangedDirectoryIterator(
             folder, false, "*", juce::File::findFilesAndDirectories)) {
        const auto file = entry.getFile();
        if (file.isSymbolicLink()) continue;
        if (file.isDirectory()) {
            // A folder ending in .vst3 IS the bundle (VST3 folder layout):
            // record it and do not descend — the inner binary
            // (Contents\x86_64-win\*.vst3) is not a separate plugin and
            // must never be scanned or instantiated on its own.
            if (file.getFileName().endsWithIgnoreCase(".vst3")) {
                bundles.addIfNotAlreadyThere(file);
                if (bundles.size() >= kMaxScanBundles) return false;
            } else if (!collectBundles(file, bundles, depth + 1)) {
                return false;
            }
        } else if (file.hasFileExtension(".vst3")) {
            bundles.addIfNotAlreadyThere(file);
            if (bundles.size() >= kMaxScanBundles) return false;
        }
    }
    return true;
}

juce::int64 cachedModTimeFor(const juce::Array<juce::PluginDescription>& plugins,
                             const juce::String& path) {
    // A bundle can hold several classes (Reaktor 6 + Reaktor 6 FX share one
    // file): return the NEWEST stamp across its entries so one stale twin
    // cannot force a rescan after its sibling just verified the bundle.
    juce::int64 newest = 0;
    for (const auto& description : plugins)
        if (description.fileOrIdentifier == path)
            newest = juce::jmax(newest, description.lastFileModTime.toMilliseconds());
    return newest;
}

// True when the on-disk bundle matches the cached description: same file
// time within filesystem granularity. Centralises the tolerance so the
// scan skip and the cache filter below can never drift apart again.
//
// Two stamp modes share this compare:
// - file-mtime stamps (old caches): disk-vs-cache within 5 s means
//   "untouched since the scan that wrote this entry".
// - scan-time stamps (new caches): the entry holds the time of the scan
//   that verified it, which always trails the older disk mtime. A bundle
//   is "unchanged" when its disk mtime is OLDER than the stamp (nothing
//   modified it since we verified it) within the same tolerance; a disk
//   mtime NEWER than the stamp means it changed after our scan.
bool bundleUnchanged(const juce::File& file, juce::int64 cachedMs) {
    if (cachedMs <= 0) return false;
    const auto diskMs = file.getLastModificationTime().toMilliseconds();
    if (diskMs <= 0) return false;
    // Disk older-or-equal (within tolerance): verified after last change.
    if (diskMs <= cachedMs + 5000) return true;
    // Disk newer by more than the tolerance: real modification.
    return false;
}

void removeFileEntries(juce::Array<juce::PluginDescription>& plugins, const juce::String& path) {
    for (int i = plugins.size(); --i >= 0;)
        if (plugins.getReference(i).fileOrIdentifier == path) plugins.remove(i);
}

void sortPlugins(juce::Array<juce::PluginDescription>& plugins) {
    std::sort(plugins.begin(), plugins.end(),
              [](const juce::PluginDescription& a, const juce::PluginDescription& b) {
                  const auto vendorA = PluginLibrary::vendorNameFor(a);
                  const auto vendorB = PluginLibrary::vendorNameFor(b);
                  if (vendorA.compareIgnoreCase(vendorB) != 0)
                      return caseInsensitiveLess(vendorA, vendorB);
                  const auto nameA = PluginLibrary::displayNameFor(a);
                  const auto nameB = PluginLibrary::displayNameFor(b);
                  if (nameA.compareIgnoreCase(nameB) != 0)
                      return caseInsensitiveLess(nameA, nameB);
                  return caseInsensitiveLess(a.fileOrIdentifier, b.fileOrIdentifier);
              });
}

std::unique_ptr<juce::XmlElement> buildCacheXml(
    const juce::Array<juce::PluginDescription>& plugins,
    const juce::Array<juce::File>& folders) {
    auto root = std::make_unique<juce::XmlElement>("PLUGINRACK_PLUGIN_LIBRARY");
    auto* folderList = root->createNewChildElement("FOLDERS");
    for (const auto& folder : folders)
        folderList->createNewChildElement("FOLDER")
            ->setAttribute("path", folder.getFullPathName());
    auto* pluginList = root->createNewChildElement("PLUGINS");
    for (const auto& description : plugins)
        pluginList->addChildElement(description.createXml().release());
    return root;
}

void readCacheXml(const juce::XmlElement& root,
                  juce::Array<juce::PluginDescription>& plugins,
                  juce::Array<juce::File>& folders) {
    if (const auto* folderList = root.getChildByName("FOLDERS"))
        for (const auto* folder : folderList->getChildWithTagNameIterator("FOLDER"))
            folders.addIfNotAlreadyThere(juce::File(folder->getStringAttribute("path")));
    if (const auto* pluginList = root.getChildByName("PLUGINS"))
        for (const auto* element : pluginList->getChildWithTagNameIterator("PLUGIN")) {
            juce::PluginDescription description;
            if (description.loadFromXml(*element) && description.fileOrIdentifier.isNotEmpty())
                plugins.add(description);
        }
}

} // namespace

PluginLibrary::PluginLibrary() = default;

PluginLibrary::~PluginLibrary() {
    alive->store(false);
    // Waiting here is what lets the job keep using `this` for the whole scan.
    pool.removeAllJobs(true, 30000);
}

PluginLibrary& PluginLibrary::getInstance() {
    // Process lifetime, and owned by DeletedAtShutdown so the scan thread is
    // wound down while the message loop is still alive.
    static auto* instance = new PluginLibrary();
    return *instance;
}

juce::File PluginLibrary::cacheFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("PluginRack")
        .getChildFile("plugins")
        .getChildFile("plugin-library.xml");
}

juce::String PluginLibrary::vendorNameFor(const juce::PluginDescription& description) {
    const auto vendor = description.manufacturerName.trim();
    if (vendor.isNotEmpty()) return vendor;
    // Bundles without manufacturer info usually sit in a vendor folder:
    // <VST3 folder>/<Vendor>/<Plugin>.vst3.
    const juce::File file(description.fileOrIdentifier);
    if (juce::File::isAbsolutePath(file.getFullPathName())) {
        const auto parent = file.getParentDirectory().getFileName();
        if (parent.isNotEmpty() && parent != "VST3") return parent;
    }
    return "Other";
}

juce::String PluginLibrary::displayNameFor(const juce::PluginDescription& description) {
    const auto descriptive = description.descriptiveName.trim();
    if (descriptive.isNotEmpty()) return descriptive;
    if (description.name.trim().isNotEmpty()) return description.name.trim();
    return juce::File(description.fileOrIdentifier).getFileNameWithoutExtension();
}

bool PluginLibrary::isTrustedPluginPath(const juce::String& path) {
    if (path.isEmpty()) return false;
    const juce::File file(path);
    // Exact library hit (case-insensitive): the scanned bundle itself.
    {
        const auto plugins = getInstance().getPlugins();
        for (const auto& description : plugins) {
            if (description.fileOrIdentifier.compareIgnoreCase(path) == 0) return true;
        }
    }
    // Otherwise: under a scan folder (platform defaults + explicit extras).
    // Anything else (Downloads, a share, a scene-named absolute path) is
    // untrusted and gets the warn-and-confirm gate in the UI.
    const auto folders = getInstance().getScanFolders();
    for (const auto& folder : folders) {
        if (file.isAChildOf(folder)) return true;
    }
    return false;
}

bool PluginLibrary::isScanning() const {
    return scanning.load();
}

bool PluginLibrary::hasScanned() const {
    return scanCompleted.load();
}

juce::Array<juce::PluginDescription> PluginLibrary::getPlugins() const {
    const juce::ScopedLock lock(listLock);
    return plugins;
}

int PluginLibrary::getNumPlugins() const {
    const juce::ScopedLock lock(listLock);
    return plugins.size();
}

juce::String PluginLibrary::getStatusText() const {
    if (scanning.load()) {
        juce::String name;
        {
            const juce::ScopedLock lock(progressLock);
            name = progressName;
        }
        const auto total = progressTotal.load();
        if (total <= 0) return "Scanning plugin folders...";
        return "Scanning " + juce::String(progressDone.load()) + "/" + juce::String(total)
               + (name.isNotEmpty() ? " - " + name : juce::String());
    }
    const auto count = getNumPlugins();
    return count == 1 ? juce::String("1 plugin found")
                      : juce::String(count) + " plugins found";
}

juce::Array<juce::File> PluginLibrary::getExtraFolders() const {
    const juce::ScopedLock lock(listLock);
    return extraFolders;
}

void PluginLibrary::addExtraFolder(const juce::File& folder) {
    if (!folder.isDirectory()) return;
    {
        const juce::ScopedLock lock(listLock);
        extraFolders.addIfNotAlreadyThere(folder);
    }
    saveCache();
}

juce::Array<juce::File> PluginLibrary::getScanFolders() const {
    juce::Array<juce::File> folders;

    // The format owns the platform default paths (and VST3_PATH on Windows),
    // so the scanner follows whatever JUCE considers installed.
    juce::VST3PluginFormat format;
    const auto defaults = format.getDefaultLocationsToSearch();
    for (int i = 0; i < defaults.getNumPaths(); ++i) {
        const juce::File folder(defaults[i]);
        if (folder.isDirectory()) folders.addIfNotAlreadyThere(folder);
    }

    const auto extras = getExtraFolders();
    for (const auto& folder : extras)
        if (folder.isDirectory()) folders.addIfNotAlreadyThere(folder);
    return folders;
}

void PluginLibrary::ensureCacheLoaded() {
    const juce::ScopedLock loadLock(cacheLoadLock);
    if (cacheLoaded) return;
    cacheLoaded = true;

    const auto xml = juce::XmlDocument::parse(cacheFile());
    if (xml == nullptr) return;

    juce::Array<juce::PluginDescription> loadedPlugins;
    juce::Array<juce::File> loadedFolders;
    readCacheXml(*xml, loadedPlugins, loadedFolders);
    // The cache is advisory. Never restore scan roots from user-writable XML,
    // and only retain entries that still point at an existing bundle below a
    // platform-provided scan root.
    loadedFolders.clear();
    juce::Array<juce::File> trustedRoots;
    juce::VST3PluginFormat format;
    const auto defaults = format.getDefaultLocationsToSearch();
    for (int i = 0; i < defaults.getNumPaths(); ++i)
        trustedRoots.add(juce::File(defaults[i]));
    for (int i = loadedPlugins.size(); --i >= 0;) {
        const juce::File file(loadedPlugins.getReference(i).fileOrIdentifier);
        bool underTrustedRoot = false;
        for (const auto& root : trustedRoots)
            if (file.isAChildOf(root)) {
                underTrustedRoot = true;
                break;
            }
        // Bundles are files (.vst3) OR folders (*.vst3): existsAsFile is
        // false for a folder bundle, so accept either. Without this every
        // folder bundle is dropped from the cache on load, the trust gate
        // sees an empty library, and every save load warn-and-confirms
        // (or bypasses at startup) while the background scan re-does all
        // 53 bundles.
        const bool exists = file.existsAsFile() || file.isDirectory();
        if (!file.isAbsolutePath(file.getFullPathName()) || !underTrustedRoot || !exists
            || file.isSymbolicLink())
            loadedPlugins.remove(i);
    }
    sortPlugins(loadedPlugins);

    const juce::ScopedLock lock(listLock);
    plugins = loadedPlugins;
    for (const auto& folder : loadedFolders) extraFolders.addIfNotAlreadyThere(folder);
}

void PluginLibrary::saveCache() {
    juce::Array<juce::PluginDescription> pluginCopy;
    juce::Array<juce::File> folderCopy;
    {
        const juce::ScopedLock lock(listLock);
        pluginCopy = plugins;
        folderCopy = extraFolders;
    }

    const juce::ScopedLock lock(saveLock);
    const auto file = cacheFile();
    file.getParentDirectory().createDirectory();
    buildCacheXml(pluginCopy, folderCopy)->writeTo(file);
}

void PluginLibrary::runScan(const juce::File& onlyFolder, bool clearAll, bool force) {
    juce::Array<juce::File> folders;
    if (onlyFolder != juce::File()) folders.add(onlyFolder);
    else folders = getScanFolders();

    juce::Array<juce::File> bundles;
    for (const auto& folder : folders) {
        if (!collectBundles(folder, bundles, 0)) {
            juce::Logger::writeToLog("plugin scan: bundle limit reached");
            break;
        }
    }
    std::sort(bundles.begin(), bundles.end(),
              [](const juce::File& a, const juce::File& b) {
                  return caseInsensitiveLess(a.getFullPathName(), b.getFullPathName());
              });

    juce::Array<juce::PluginDescription> work;
    if (!clearAll) work = getPlugins();

    progressTotal = bundles.size();
    progressDone = 0;
    int rescanned = 0;
    int skipped = 0;
    juce::StringArray rescannedNames;

    for (int index = 0; index < bundles.size(); ++index) {
        if (!alive->load()) return;
        if (auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob())
            if (job->shouldExit()) return;

        const auto file = bundles.getReference(index);
        const auto path = file.getFullPathName();
        {
            const juce::ScopedLock lock(progressLock);
            progressName = file.getFileName();
        }
        progressDone = index;

        // An unchanged bundle keeps the descriptions the cache already holds.
        // Tolerance, not exact match: filesystem granularity and copy tools
        // shift sub-second parts, and an exact compare forces a full rescan
        // of every bundle on every launch (see bundleUnchanged). The walk
        // yields bundle roots (never inner binaries), matching the path
        // the scan reports, so a plain string lookup is correct.
        if (!force && bundleUnchanged(file, cachedModTimeFor(work, path))) {
            ++skipped;
            continue;
        }
        ++rescanned;
        if (rescannedNames.size() < 10) rescannedNames.add(file.getFileName());

        if (PluginLoadGuard::isBlocked(file)) {
            juce::Logger::writeToLog("plugin scan: skipped blocked " + file.getFileName());
            removeFileEntries(work, path);
            continue;
        }

        juce::String error;
        const auto found = PluginHost::scanFile(file, error);
        removeFileEntries(work, path);
        if (found.isEmpty()) {
            juce::Logger::writeToLog(
                "plugin scan: " + file.getFileName() + " - "
                + (error.isNotEmpty() ? error : juce::String("no plugin class in the bundle")));
            continue;
        }
        for (const auto& description : found) {
            auto entry = description;
            // Normalise to the bundle root: the scan reports whatever path
            // it was handed (inner binary for old walk entries), but the
            // walk, the cache, the trust gate and the host all speak bundle
            // roots. Without this one old inner-binary cache entry never
            // matches its bundle-root walk path and rescans every launch.
            if (entry.fileOrIdentifier.containsIgnoreCase("Contents\\x86_64-win")) {
                juce::File f(entry.fileOrIdentifier);
                auto root = f;
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
                if (root.getFileName().endsWithIgnoreCase(".vst3"))
                    entry.fileOrIdentifier = root.getFullPathName();
            }
            // Stamp the scan time, not the file's mtime: the skip check
            // compares disk-vs-cache, and the disk mtime of an untouched
            // bundle never changes — re-stamping it would be a no-op while
            // stamping "now" marks this entry verified-this-run. A bundle
            // whose disk mtime moves past the tolerance is still rescanned.
            entry.lastFileModTime = juce::Time::getCurrentTime();
            entry.lastInfoUpdateTime = juce::Time::getCurrentTime();
            work.add(entry);
        }
    }

    // A bundle that was uninstalled would otherwise stay in the menu forever.
    for (int i = work.size(); --i >= 0;) {
        const auto& path = work.getReference(i).fileOrIdentifier;
        if (juce::File::isAbsolutePath(path) && !juce::File(path).exists()) work.remove(i);
    }

    progressDone = bundles.size();
    sortPlugins(work);
    {
        const juce::ScopedLock lock(listLock);
        plugins = work;
    }
    saveCache();
    scanCompleted = true;
    juce::Logger::writeToLog("plugin scan: " + juce::String(work.size()) + " description(s), "
                             + juce::String(rescanned) + " rescanned, " + juce::String(skipped)
                             + " skipped");
    std::cout << "plugin scan: " << work.size() << " description(s), " << rescanned
              << " rescanned, " << skipped << " skipped\n";
    if (rescanned > 0) {
        std::cout << "rescanned: " << rescannedNames.joinIntoString(", ").toStdString() << "\n";
    }
    std::cout.flush();
}

void PluginLibrary::startScan(const juce::File& onlyFolder, bool clearAll, bool force) {
    if (scanning.exchange(true)) {
        rescanQueued = true;
        return;
    }

    progressDone = 0;
    progressTotal = 0;
    {
        const juce::ScopedLock lock(progressLock);
        progressName.clear();
    }

    const auto keepAlive = alive;
    pool.addJob([this, keepAlive, onlyFolder, clearAll, force] {
        runScan(onlyFolder, clearAll, force);
        juce::MessageManager::callAsync([this, keepAlive] {
            if (!keepAlive->load()) return;
            scanning = false;
            {
                const juce::ScopedLock lock(progressLock);
                progressName.clear();
            }
            sendChangeMessage();
            // Always a full pass: a folder added while the scan was running was
            // not in that scan's folder list.
            if (rescanQueued.exchange(false)) startScan({}, true, true);
        });
    });
}

void PluginLibrary::beginBackgroundScan() {
    ensureCacheLoaded();
    startScan({}, false, false);
}

void PluginLibrary::rescan(const juce::File& onlyFolder) {
    ensureCacheLoaded();
    startScan(onlyFolder, onlyFolder == juce::File(), true);
}

int PluginLibrary::scanBlocking(const juce::File& onlyFolder) {
    ensureCacheLoaded();
    scanning = true;
    runScan(onlyFolder, onlyFolder == juce::File(), true);
    scanning = false;
    return getNumPlugins();
}
