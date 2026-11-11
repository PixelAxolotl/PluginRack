#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <memory>

// Catalogue of the plugins installed on this machine.
//
// The plugin folders are walked for bundles and each one is described by
// PluginHost::scanFile, which maps the plugin in a throwaway child process:
// a plugin that faults while enumerating its classes cannot take the app down.
// Results are cached in %APPDATA%/PluginRack/plugins/plugin-library.xml, so a
// later launch only rescans the bundles that changed on disk.
class PluginLibrary final : public juce::ChangeBroadcaster, private juce::DeletedAtShutdown {
public:
    static PluginLibrary& getInstance();

    // Loads the cache once, then refreshes it in the background. Message thread.
    void beginBackgroundScan();
    // Rescans everything, or just one folder, in the background. A scan that is
    // already running queues one more round instead of starting a second one.
    void rescan(const juce::File& onlyFolder = {});
    // Blocking. Returns the number of descriptions found; --list-plugins only.
    int scanBlocking(const juce::File& onlyFolder = {});

    bool isScanning() const;
    // True once a scan finished in this session, so the menu does not restart
    // one every time it is opened.
    bool hasScanned() const;
    // Copy taken under the list lock, so the menu can be built mid-scan.
    juce::Array<juce::PluginDescription> getPlugins() const;
    int getNumPlugins() const;
    // "Scanning 3/8 - Raum.vst3", or "12 plugins found".
    juce::String getStatusText() const;

    juce::Array<juce::File> getExtraFolders() const;
    void addExtraFolder(const juce::File& folder);
    // Standard plugin folders plus the user's, in scan order.
    juce::Array<juce::File> getScanFolders() const;

    // The vendor a plugin is listed under: its manufacturer, else the folder it
    // sits in, else "Other".
    static juce::String vendorNameFor(const juce::PluginDescription& description);
    // The name a plugin is listed under.
    static juce::String displayNameFor(const juce::PluginDescription& description);
    // PR-SEC-010: true when the path names a scanned library bundle (or a
    // bundle under an explicit scan folder). Scene restore warn-and-confirms
    // anything else instead of loading it silently.
    static bool isTrustedPluginPath(const juce::String& path);

    static juce::File cacheFile();

private:
    PluginLibrary();
    ~PluginLibrary() override;

    void ensureCacheLoaded();
    // Runs on the calling thread. `force` rescans bundles whose file time is
    // unchanged; `clearAll` drops descriptions of plugins no longer installed.
    void runScan(const juce::File& onlyFolder, bool clearAll, bool force);
    void startScan(const juce::File& onlyFolder, bool clearAll, bool force);
    void saveCache();

    mutable juce::CriticalSection listLock;
    juce::Array<juce::PluginDescription> plugins;
    juce::Array<juce::File> extraFolders;

    juce::CriticalSection cacheLoadLock;
    bool cacheLoaded = false;

    std::atomic<bool> scanning{false};
    std::atomic<bool> scanCompleted{false};
    std::atomic<bool> rescanQueued{false};
    std::atomic<int> progressDone{0};
    std::atomic<int> progressTotal{0};
    mutable juce::CriticalSection progressLock;
    juce::String progressName;

    // The destructor waits for the scan job, which keeps `this` valid for its
    // whole run; the flag keeps a late completion off an already deleted object.
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    juce::CriticalSection saveLock;
    juce::ThreadPool pool{juce::ThreadPoolOptions{}
                              .withNumberOfThreads(1)
                              .withThreadName("PluginRack scan")};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginLibrary)
};
