#include <JuceHeader.h>

#include "ConsoleLookAndFeel.h"
#include "DeviceStream.h"
#include "MainComponent.h"
#include "PluginHost.h"
#include "PluginLibrary.h"
#include "SliderTouch.h"

#include <cstdlib>
#include <iostream>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace {

// Run log and crash reports. A Windows crash record only gives the module and
// a code offset, so the report also carries the fault address and a backtrace;
// with the PDB this build ships next to the exe, llvm-symbolizer turns those
// into function names and source lines.
juce::File pluginRackLogDirectory() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("PluginRack")
        .getChildFile("logs");
}

// Every line carries the milliseconds since the run started, so a stall shows
// up as a jump between two lines without needing a profiler.
class TimestampedLogger final : public juce::Logger {
public:
    TimestampedLogger(const juce::File& file, const juce::String& startMessage)
        : sink(file, startMessage), startMs(juce::Time::getMillisecondCounterHiRes()) {}

    void logMessage(const juce::String& message) override {
        const auto elapsed = juce::Time::getMillisecondCounterHiRes() - startMs;
        sink.logMessage(juce::String(elapsed, 1).paddedLeft(' ', 9) + " ms  " + message);
    }

private:
    juce::FileLogger sink;
    double startMs = 0.0;
};

void writeCrashReport(void* exceptionInfo) {
    juce::String text;
    text << "PluginRack crash\n"
         << "when: " << juce::Time::getCurrentTime().toString(true, true, true, true) << "\n"
         << "build: " << __DATE__ << " " << __TIME__ << "\n";

   #if JUCE_WINDOWS
    if (auto* info = static_cast<EXCEPTION_POINTERS*>(exceptionInfo)) {
        if (auto* record = info->ExceptionRecord) {
            text << "exception: 0x"
                 << juce::String::toHexString((juce::int64) record->ExceptionCode)
                 << " at 0x"
                 << juce::String::toHexString(
                        (juce::int64) reinterpret_cast<juce::pointer_sized_int>(
                            record->ExceptionAddress))
                 << "\n";
        }
    }
   #endif

    // A plugin that faults in its own code can only be identified after the
    // fact: record which file was in flight and refuse to load it next time.
    const auto loadingPlugin = PluginLoadGuard::currentLoadingPlugin();
    if (loadingPlugin.isNotEmpty()) {
        text << "plugin: " << loadingPlugin << "\n";
        PluginLoadGuard::block(juce::File(loadingPlugin));
    }

    const auto backtrace = juce::SystemStats::getStackBacktrace();
    text << "stack:\n" << backtrace << "\n";
    std::cerr << text.toStdString() << std::endl;

    const auto dir = pluginRackLogDirectory();
    dir.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    dir.getChildFile("crash-" + stamp + ".txt").replaceWithText(text);
}

} // namespace

class TrayIcon final : public juce::SystemTrayIconComponent {
public:
    TrayIcon(std::function<void()> onShowWindow, std::function<void()> onQuit)
        : onShowWindow_(std::move(onShowWindow)), onQuit_(std::move(onQuit)) {
        juce::Image image(juce::Image::ARGB, 32, 32, true);
        juce::Graphics g(image);
        g.fillAll(juce::Colours::transparentBlack);
        g.setColour(juce::Colours::limegreen);
        g.fillEllipse(4.0f, 4.0f, 24.0f, 24.0f);
        setIconImage(image, image);
        setIconTooltip("PluginRack");
    }

    void mouseDown(const juce::MouseEvent& e) override {
        // Right click (or ctrl-click) opens the menu. While the window is
        // hidden in the tray that menu is the only way to quit, so it must not
        // share the left-click path that only restores the window.
        if (e.mods.isPopupMenu()) {
            showMenu();
            return;
        }
        if (onShowWindow_) onShowWindow_();
    }

private:
    void showMenu() {
        juce::PopupMenu menu;
        menu.addItem(1, "Open PluginRack");
        menu.addSeparator();
        menu.addItem(2, "Quit PluginRack");
        juce::Component::SafePointer<TrayIcon> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
                           [safe](int result) {
                               if (safe == nullptr) return;
                               if (result == 1 && safe->onShowWindow_) safe->onShowWindow_();
                               else if (result == 2 && safe->onQuit_) safe->onQuit_();
                           });
    }

    std::function<void()> onShowWindow_;
    std::function<void()> onQuit_;
};

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(bool closeToTray, std::function<void(bool)> onCloseToTrayChanged)
        : juce::DocumentWindow("PluginRack", juce::Colours::darkgrey, allButtons)
        , closeToTray_(closeToTray) {
        setUsingNativeTitleBar(true);
        auto* content = new MainComponent();
        content->settings().onCloseToTrayChanged = [this, onCloseToTrayChanged](bool enabled) {
            closeToTray_ = enabled;
            if (onCloseToTrayChanged) onCloseToTrayChanged(enabled);
        };
        content->settings().setCloseToTrayEnabled(closeToTray_);
        setContentOwned(content, true);
        trayIcon_ = std::make_unique<TrayIcon>(
            [this] {
                setVisible(true);
                toFront(true);
            },
            [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
        setResizeLimits(1180, 700, 10000, 10000);
        centreWithSize(1600, 900);
        setVisible(true);
    }

    void closeButtonPressed() override {
        if (closeToTray_) {
            setVisible(false);
            return;
        }
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    bool closeToTray_ = true;
    std::unique_ptr<TrayIcon> trayIcon_;
};

class PluginRackApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "PluginRack"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    // One session per machine, with one deliberate exception: the plugin scanner
    // and the reporting switches run this same executable as short-lived child
    // processes. Blocking those would break scanning the moment the app is open,
    // so any '--' mode stays outside the single-instance lock and only the GUI
    // takes it.
    bool moreThanOneInstanceAllowed() override {
        for (const auto& arg : juce::JUCEApplicationBase::getCommandLineParameterArray()) {
            if (arg.startsWith("--")) return true;
        }
        return false;
    }

    // Called in the running instance when the user launches the app again.
    void anotherInstanceStarted(const juce::String& commandLine) override {
        juce::ignoreUnused(commandLine);
        if (mainWindow == nullptr) return;
        // Show the session that already exists instead of doing nothing: the
        // window may be minimised, or hidden in the tray.
        mainWindow->setVisible(true);
        if (mainWindow->isMinimised()) mainWindow->setMinimised(false);
        mainWindow->toFront(true);
        juce::Logger::writeToLog("second launch: focused the running window");
    }

    void initialise(const juce::String& commandLine) override {
        juce::SystemStats::setApplicationCrashHandler(&writeCrashReport);

        juce::StringArray args;
        args.addTokens(commandLine, true);

        // Child-process scan: reads the plugin's descriptions in a throwaway
        // process so the main window never loads a plugin binary while it is
        // starting up (see PluginHost::scanFile). No window, no audio, and no
        // run log: a crash here is recorded by the handler above.
        const int scanIndex = args.indexOf("--scan-plugin");
        if (scanIndex >= 0) {
            juce::String scanError;
            const auto descriptions =
                scanIndex + 1 < args.size()
                    ? PluginHost::scanFileInProcess(juce::File(args[scanIndex + 1].unquoted()),
                                                    scanError)
                    : juce::Array<juce::PluginDescription>{};
            for (const auto& description : descriptions) {
                std::cout << "PLUGINDESC " << PluginHost::describeAsJson(description).toStdString()
                          << "\n";
            }
            if (descriptions.isEmpty() && scanError.isNotEmpty()) {
                std::cout << "PLUGINSCANERROR " << scanError.toStdString() << "\n";
            }
            std::cout.flush();
            quit();
            return;
        }

        pluginRackLogDirectory().createDirectory();
        fileLogger = std::make_unique<TimestampedLogger>(
            pluginRackLogDirectory().getChildFile("PluginRack.log"), "PluginRack started");
        juce::Logger::setCurrentLogger(fileLogger.get());
        juce::Logger::writeToLog("initialise: " + commandLine);

        if (args.contains("--clear-blocked-plugins")) {
            const auto cleared = PluginLoadGuard::clearBlocked();
            std::cout << "Cleared " << cleared << " blocked plugin(s) from "
                      << PluginLoadGuard::blockedListFile().getFullPathName().toStdString()
                      << "\n";
            quit();
            return;
        }

        if (args.contains("--list-plugins")) {
            auto& library = PluginLibrary::getInstance();
            library.scanBlocking();
            const auto plugins = library.getPlugins();
            std::cout << plugins.size() << " plugin(s) found:\n";
            juce::String currentVendor;
            for (const auto& description : plugins) {
                const auto vendor = PluginLibrary::vendorNameFor(description);
                if (vendor != currentVendor) {
                    currentVendor = vendor;
                    std::cout << "  " << vendor.toStdString() << "\n";
                }
                std::cout << "    " << PluginLibrary::displayNameFor(description).toStdString()
                          << " (" << description.pluginFormatName.toStdString() << ") ["
                          << description.fileOrIdentifier.toStdString() << "]\n";
            }
            if (plugins.isEmpty()) std::cout << "  (none)\n";
            std::cout.flush();
            quit();
            return;
        }

        if (args.contains("--list-devices")) {
            const auto inNames = DeviceEnumeration::inputs();
            const auto outNames = DeviceEnumeration::outputs();
            std::cout << "Input devices:\n";
            for (const auto& n : inNames) std::cout << "  " << n.toStdString() << "\n";
            if (inNames.isEmpty()) std::cout << "  (none)\n";

            std::cout << "Output devices:\n";
            for (const auto& n : outNames) std::cout << "  " << n.toStdString() << "\n";
            if (outNames.isEmpty()) std::cout << "  (none)\n";

            quit();
            return;
        }

        int idx = args.indexOf("--screenshot");
        if (idx >= 0 && idx + 1 < args.size()) {
            const juce::File outPath(args[idx + 1]);
            outPath.getParentDirectory().createDirectory();
            // Positional size args stay backward compatible; any known view
            // name among the trailing args selects the captured view.
            int shotW = 1600;
            int shotH = 900;
            bool haveW = false;
            AppView shotView = AppView::Rack;
            for (int a = idx + 2; a < args.size(); ++a) {
                const auto tok = args[a].toLowerCase();
                if (tok.isEmpty()) continue;
                if (tok == "mixer") shotView = AppView::Mixer;
                else if (tok == "rack" || tok == "racks") shotView = AppView::Rack;
                else if (tok == "routing") shotView = AppView::Routing;
                else if (tok == "io" || tok == "i/o") shotView = AppView::IO;
                else if (tok == "saves") shotView = AppView::Saves;
                else if (tok == "settings") shotView = AppView::Settings;
                else if (tok.containsOnly("0123456789")) {
                    if (!haveW) { shotW = juce::jmax(1, tok.getIntValue()); haveW = true; }
                    else shotH = juce::jmax(1, tok.getIntValue());
                }
            }
            lookAndFeel = std::make_unique<ConsoleLookAndFeel>();
            juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());
            auto* comp = new MainComponent();
            comp->selectView(shotView);
            comp->setSize(shotW, shotH);
            juce::Image snapshot = comp->createComponentSnapshot(comp->getLocalBounds(), false, 1.0f);
            juce::PNGImageFormat png;
            if (auto stream = outPath.createOutputStream()) {
                png.writeImageToStream(snapshot, *stream);
            }
            delete comp;
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
            lookAndFeel.reset();
            quit();
            return;
        }

        lookAndFeel = std::make_unique<ConsoleLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());
        tooltipWindow = std::make_unique<juce::TooltipWindow>(nullptr, 100);
        // Right-click value entry for dials that have no value box of their own,
        // and last-touched tracking for the rack's touch-and-slide strip.
        slider_touch::installRightClickEditor();
        juce::PropertiesFile::Options storageOptions;
        storageOptions.applicationName = "PluginRack";
        storageOptions.filenameSuffix = ".settings";
        storageOptions.folderName = "PluginRack";
        storageOptions.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters(storageOptions);
        const bool closeToTray = properties.getUserSettings()->getBoolValue("closeToTray", true);
        mainWindow = std::make_unique<MainWindow>(closeToTray, [this](bool enabled) {
            properties.getUserSettings()->setValue("closeToTray", enabled);
            properties.getUserSettings()->saveIfNeeded();
        });

        // Diagnostic: drives the real Add-VST path (the same call the rack's
        // picker makes) once the window and audio are up, then reports whether
        // the message thread came back.
        const int addStart = args.indexOf("--test-add");
        if (addStart >= 0 && addStart + 1 < args.size()) {
            const juce::File addFile(args[addStart + 1].unquoted());
            juce::Timer::callAfterDelay(8000, [this, addFile] {
                juce::Logger::writeToLog("test-add: inserting " + addFile.getFileName());
                if (auto* component =
                        dynamic_cast<MainComponent*>(mainWindow->getContentComponent())) {
                    component->rack().insertPlugin(addFile);
                }
                juce::Timer::callAfterDelay(15000, [] {
                    juce::Logger::writeToLog("test-add: still alive after 15 s, quitting");
                    if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
                });
            });
        }

        // Plugin smoke test: loads one VST3 the way the Add-VST path does
        // (message thread, audio already running), reports the result and quits.
        // Useful when a plugin is suspected of crashing the host or of stalling
        // the window; the timings land in the run log. The probe follows the
        // Add-VST path exactly: scan in a child process on a worker, create on
        // the message thread.
        const int testStart = args.indexOf("--test-plugin");
        const bool probeWithAudio = args.contains("--test-plugin-audio");
        if (testStart >= 0 && testStart + 1 < args.size()) {
            const juce::File testFile(args[testStart + 1].unquoted());
            juce::Timer::callAfterDelay(5000, [testFile, probeWithAudio] {
                juce::Logger::writeToLog("plugin: test load " + testFile.getFileName());
                juce::Thread::launch([testFile, probeWithAudio] {
                    juce::String scanError;
                    const auto descriptions = PluginHost::scanFile(testFile, scanError);
                    juce::MessageManager::callAsync([testFile, descriptions, scanError,
                                                     probeWithAudio] {
                        const auto blockedStart = juce::Time::getMillisecondCounterHiRes();
                        juce::String result;
                        if (descriptions.isEmpty()) {
                            result = "failed - "
                                   + (scanError.isNotEmpty()
                                          ? scanError
                                          : juce::String("No VST3 plugin was found in the selected file."));
                        } else {
                            PluginHost probe;
                            const auto loaded = PluginHost::instantiate(
                                descriptions.getReference(0), testFile, 48000.0, 480);
                            if (loaded.error.isNotEmpty()) {
                                result = "failed - " + loaded.error;
                            } else {
                                probe.prepareToPlay(48000.0, 480);
                                probe.adopt(loaded);
                                // Optional: run blocks through the plugin while
                                // its editor is created, which is what happens
                                // when a plugin is added mid-session.
                                std::atomic<bool> pumping{probeWithAudio};
                                std::thread audioThread;
                                if (probeWithAudio) {
                                    juce::Logger::writeToLog("plugin: test audio pumping");
                                    audioThread = std::thread([&probe, &pumping] {
                                        juce::AudioBuffer<float> block(2, 480);
                                        while (pumping.load(std::memory_order_relaxed)) {
                                            block.clear();
                                            probe.processBlock(block);
                                        }
                                    });
                                    juce::Thread::sleep(300);
                                }
                                // The rack docks an editor right after a plugin is
                                // added, so the probe measures that too. Building
                                // it is the last thing the add path does; tearing
                                // it down is slow for some plugins and is not part
                                // of adding, so it happens after the timing.
                                auto* editor = probe.createEditorForDock();
                                const auto blocked =
                                    juce::Time::getMillisecondCounterHiRes() - blockedStart;
                                result = "loaded " + testFile.getFileName()
                                       + ", message thread blocked for "
                                       + juce::String(blocked, 1) + " ms";
                                juce::Logger::writeToLog("plugin: test " + result);
                                // Dock and paint it the way the rack does: a
                                // plugin whose editor paints on the message thread
                                // stalls there, which a create-only probe misses.
                                if (editor != nullptr) {
                                    // The rack scales a docked editor to fit its
                                    // bay, which only works for plugins that
                                    // honour host content scaling: report both
                                    // sizes so a plugin that ignores it is easy
                                    // to spot.
                                    const auto naturalWidth = editor->getWidth();
                                    const auto naturalHeight = editor->getHeight();
                                    editor->setScaleFactor(0.8f);
                                    const auto scaledWidth = editor->getWidth();
                                    const auto scaledHeight = editor->getHeight();
                                    editor->setScaleFactor(1.0f);
                                    juce::Logger::writeToLog(
                                        "plugin: test editor "
                                        + juce::String(naturalWidth) + "x"
                                        + juce::String(naturalHeight)
                                        + ", at host scale 0.8 "
                                        + juce::String(scaledWidth) + "x"
                                        + juce::String(scaledHeight));
                                    juce::Component holder;
                                    holder.setSize(juce::jmax(320, naturalWidth),
                                                   juce::jmax(240, naturalHeight));
                                    holder.addAndMakeVisible(editor);
                                    editor->setBounds(holder.getLocalBounds());
                                    juce::Logger::writeToLog("plugin: test editor docked");
                                    juce::Image snapshot = holder.createComponentSnapshot(
                                        holder.getLocalBounds(), true, 1.0f);
                                    juce::Logger::writeToLog(
                                        "plugin: test editor painted "
                                        + juce::String(snapshot.getWidth()) + "x"
                                        + juce::String(snapshot.getHeight()));
                                    holder.removeChildComponent(editor);
                                }
                                pumping.store(false, std::memory_order_relaxed);
                                if (audioThread.joinable()) audioThread.join();
                                juce::Logger::writeToLog("plugin: test audio stopped");
                                if (editor != nullptr) probe.destroyDockEditor(editor);
                                std::cout << result.toStdString() << "\n";
                                if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
                                return;
                            }
                        }
                        juce::Logger::writeToLog("plugin: test " + result);
                        std::cout << result.toStdString() << "\n";
                        if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
                    });
                });
            });
        }
    }

    void shutdown() override {
        juce::Logger::writeToLog("shutdown");
        if (mainWindow != nullptr) {
            if (auto* mainComponent = dynamic_cast<MainComponent*>(mainWindow->getContentComponent()))
                mainComponent->saveSceneOnExit();
        }
        // Destroying a plugin is plugin code, and some VST3s (Native
        // Instruments' Raum and Supercharger among them) block forever when
        // their editor or processor is torn down; letting Windows unload their
        // DLLs faults in the same place. The scene is saved and the log is
        // written line by line, so the reliable way to close is to end the
        // process without running any of that.
        juce::Logger::writeToLog("shutdown: exiting without tearing plugins down");
#if JUCE_WINDOWS
        ::TerminateProcess(::GetCurrentProcess(), 0);
#else
        std::_Exit(0);
#endif
    }

private:
    std::unique_ptr<juce::Logger> fileLogger;
    juce::ApplicationProperties properties;
    std::unique_ptr<ConsoleLookAndFeel> lookAndFeel;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(PluginRackApplication)
