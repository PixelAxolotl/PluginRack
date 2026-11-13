#include <JuceHeader.h>

#include "AppLogos.h"
#include "ConsoleLookAndFeel.h"
#include "CrashContext.h"
#include "DeviceStream.h"
#include "MainComponent.h"
#include "PluginHost.h"
#include "PluginLibrary.h"
#include "RestrictedHost.h"
#include "SliderTouch.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
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

    // Which insert was rendering on the faulting thread. Without this a crash
    // inside a plugin names a module and an offset and nothing else, which is
    // how a fault gets blamed on the wrong plugin.
    const auto rendering = crash_context::currentRender();
    if (!rendering.empty()) {
        text << "rendering: " << juce::String(rendering) << "\n";
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
        // Tray icon: the mint-on-dark logo, scaled to the tray size, so the
        // notification area shows the app mark instead of a plain dot.
        auto logo = juce::ImageFileFormat::loadFrom(
            AppLogos::logo_dark_png, static_cast<size_t>(AppLogos::logo_dark_pngSize));
        if (logo.isValid())
            logo = logo.rescaled(32, 32, juce::Graphics::highResamplingQuality);
        else {
            logo = juce::Image(juce::Image::ARGB, 32, 32, true);
            juce::Graphics g(logo);
            g.fillAll(juce::Colours::transparentBlack);
            g.setColour(juce::Colours::limegreen);
            g.fillEllipse(4.0f, 4.0f, 24.0f, 24.0f);
        }
        setIconImage(logo, logo);
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
        // Window icon: the mint-on-dark logo, so the taskbar and Alt-Tab show
        // the app mark instead of the default JUCE icon.
        if (const auto icon = juce::ImageFileFormat::loadFrom(
                AppLogos::logo_dark_png,
                static_cast<size_t>(AppLogos::logo_dark_pngSize));
            icon.isValid())
            setIcon(icon);
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

    // One session per machine, with one deliberate exception: the plugin scanner,
    // the plugin host and the reporting switches run this same executable as
    // short-lived child processes. Blocking those would break scanning (and
    // silently turn the plugin host into a second launch that quits) the moment
    // the app is open, so any '--' mode stays outside the single-instance lock
    // and only the GUI takes it.
    bool moreThanOneInstanceAllowed() override {
        for (const auto& arg : juce::JUCEApplicationBase::getCommandLineParameterArray()) {
            if (arg == "--scan-plugin" || arg == "--list-devices" || arg == "--list-plugins"
                || arg == "--plugin-host" || arg == "--test-crash-report"
                || arg == "--test-child-host" || arg == "--test-child-null")
                return true;
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

        // Restricted plugin host (fault containment): a VST3 runs here with
        // reduced rights, never in the main app. Launched only by
        // PluginProcessHost.
        // Optional 3rd arg: pluginUniqueId selects the class in multi-class
        // bundles; 4th: mapping name; 5th: base64 state ("-" when the app will
        // push it over the control channel, which is the normal case because a
        // command line cannot carry a megabyte of state); 6th/7th: sample rate
        // and block size to instantiate at.
        const int hostIndex = args.indexOf("--plugin-host");
        if (hostIndex >= 0 && hostIndex + 1 < args.size()) {
            const int uniqueId = (hostIndex + 2 < args.size())
                                     ? args[hostIndex + 2].unquoted().getIntValue()
                                     : 0;
            // Order: path, uniqueId, mapping name, state, sampleRate, blockSize.
            // The mapping name is read positionally and never empty, so a host
            // always finds its audio channel.
            const juce::String mappingName = (hostIndex + 3 < args.size())
                                                 ? args[hostIndex + 3].unquoted()
                                                 : juce::String{};
            juce::String state = (hostIndex + 4 < args.size())
                                     ? args[hostIndex + 4].unquoted()
                                     : juce::String{};
            if (state == "-") state = {};
            const double sampleRate = (hostIndex + 5 < args.size())
                                          ? args[hostIndex + 5].unquoted().getDoubleValue()
                                          : 48000.0;
            const int blockSize = (hostIndex + 6 < args.size())
                                      ? args[hostIndex + 6].unquoted().getIntValue()
                                      : 256;
            const int code = RestrictedHost::runHost(
                juce::File(args[hostIndex + 1].unquoted()), uniqueId, state, mappingName,
                sampleRate > 0.0 ? sampleRate : 48000.0, blockSize > 0 ? blockSize : 256);
            ::exit(code);
            return;
        }

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
        // PR-01 trust boundary: plugins run with the user's rights.
        juce::Logger::writeToLog("warning: VST3 plugins run with your user rights; "
                                 "only load plugins from publishers you trust");

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
            juce::String shotEffect;
            for (int a = idx + 2; a < args.size(); ++a) {
                const auto tok = args[a].toLowerCase();
                if (tok.isEmpty()) continue;
                if (tok == "mixer") shotView = AppView::Mixer;
                else if (tok == "rack" || tok == "racks") shotView = AppView::Rack;
                else if (tok == "routing") shotView = AppView::Routing;
                else if (tok == "io" || tok == "i/o") shotView = AppView::IO;
                else if (tok == "saves") shotView = AppView::Saves;
                else if (tok == "settings") shotView = AppView::Settings;
                else if (tok == "multiband" || tok == "deesser" || tok == "limiter") shotEffect = tok;
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
            // Visual diagnostics: build one built-in on the first channel,
            // focus it in the rack editor bay, and paint one timer frame so
            // the screenshot shows its traces instead of an empty pane.
            juce::Component* focusedEffect = nullptr;
            if (shotEffect.isNotEmpty() && shotView == AppView::Rack) {
                if (!comp->getChannelsForDiagnostics().empty()) {
                    const auto afterInsert = [&focusedEffect, shotEffect] {
                        if (focusedEffect != nullptr) {
                            if (shotEffect == "multiband")
                                static_cast<MultibandWindow*>(focusedEffect)->timerCallback();
                            else if (shotEffect == "deesser")
                                static_cast<DeEsserWindow*>(focusedEffect)->timerCallback();
                            else
                                static_cast<LimiterWindow*>(focusedEffect)->timerCallback();
                            focusedEffect->repaint();
                        }
                    };
                    if (shotEffect == "multiband") {
                        focusedEffect = comp->focusDiagnosticsEffect(pluginrack::ChainItemType::Multiband);
                        afterInsert();
                    } else if (shotEffect == "deesser") {
                        focusedEffect = comp->focusDiagnosticsEffect(pluginrack::ChainItemType::DeEsser);
                        afterInsert();
                    } else if (shotEffect == "limiter") {
                        focusedEffect = comp->focusDiagnosticsEffect(pluginrack::ChainItemType::Limiter);
                        afterInsert();
                    }
                }
            }
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

        // Lifecycle diagnostic: loads a named save after the first scene and
        // audio are up, then reports whether the message thread survives the
        // swap. Reproduces the editor-after-processor crash without the UI.
        const int loadStart = args.indexOf("--test-load-save");
        if (loadStart >= 0 && loadStart + 1 < args.size()) {
            const auto saveName = args[loadStart + 1].unquoted();
            juce::Timer::callAfterDelay(8000, [this, saveName] {
                juce::Logger::writeToLog("test-load-save: loading " + saveName);
                if (auto* component =
                        dynamic_cast<MainComponent*>(mainWindow->getContentComponent())) {
                    const auto error = component->loadSaveForDiagnostics(saveName);
                    juce::Logger::writeToLog(error.isEmpty()
                                                 ? "test-load-save: loaded, still alive"
                                                 : "test-load-save: " + error);
                }
                juce::Timer::callAfterDelay(15000, [] {
                    juce::Logger::writeToLog("test-load-save: still alive after 15 s, quitting");
                    if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
                });
            });
        }

        // Crash-report diagnostic: faults on purpose while an insert label is
        // published, so the report's plugin attribution can be verified end to
        // end. The crash is the test; there is no other effect.
        const int crashReportIndex = args.indexOf("--test-crash-report");
        if (crashReportIndex >= 0) {
            const auto labelFile = crashReportIndex + 1 < args.size()
                                       ? args[crashReportIndex + 1].unquoted()
                                       : juce::String("Test.vst3");
            const auto label = std::string("Vox insert 1 \"Test\" [")
                               + juce::File(labelFile).getFileName().toStdString() + "]";
            crash_context::setCurrentRender(crash_context::registerLabel(label));
            std::cout << "test-crash-report: faulting while rendering '" << label << "'\n";
            std::cout.flush();
            ::RaiseException(0xC0000005u, 0, 0, nullptr);
            return; // not reached
        }

        // Child-host null test (phase 1 gate): renders the same deterministic
        // input through an in-process instance and through the child host, then
        // compares them sample by sample. Passing proves the child path is not
        // just "non-silent" but the same DSP.
        const int nullTestIndex = args.indexOf("--test-child-null");
        if (nullTestIndex >= 0 && nullTestIndex + 1 < args.size()) {
            const juce::File nullTestFile(args[nullTestIndex + 1].unquoted());
            // Message thread: the in-process instance must be created there
            // (JUCE loads the plugin DLL on the creating thread), and so must
            // the comparison, since it renders through that instance.
            juce::MessageManager::callAsync([nullTestFile] {
                constexpr double rate = 48000.0;
                constexpr int blockSize = 256;
                constexpr int blocks = 200;

                const auto fail = [](const juce::String& result) {
                    juce::Logger::writeToLog("test-child-null: " + result);
                    std::cout << result.toStdString() << "\n";
                    std::cout.flush();
                    // Skip plugin teardown for the same reason the app's
                    // shutdown does: some VST3s block forever in their dtor.
                    ::exit(1);
                };

                // Deterministic input: a sine whose amplitude and frequency
                // drift slowly, plus an impulse every 32 samples, so both
                // linear and time-domain behaviour is exercised.
                const auto makeBlock = [rate, blockSize](int index, double& phase) {
                    juce::AudioBuffer<float> block(2, blockSize);
                    const double step = 2.0 * juce::MathConstants<double>::pi
                                        * (220.0 + 40.0 * std::sin(index * 0.01)) / rate;
                    for (int i = 0; i < blockSize; ++i) {
                        const auto gain = 0.2 + 0.1 * std::sin(index * 0.02);
                        auto sample = static_cast<float>(gain * std::sin(phase));
                        const int absolute = index * blockSize + i;
                        if (absolute % 32 == 0) sample += 0.35f;
                        block.setSample(0, i, sample);
                        block.setSample(1, i, sample * -1.0f); // channel identity
                        phase += step;
                    }
                    return block;
                };

                // In-process reference.
                auto reference = PluginHost::loadFromFile(nullTestFile, rate, blockSize);
                if (reference.error.isNotEmpty() || reference.instance == nullptr) {
                    fail("failed - in-process load: "
                         + (reference.error.isNotEmpty() ? reference.error
                                                         : juce::String("no instance")));
                }
                PluginHost inProcess;
                inProcess.prepareToPlay(rate, blockSize);
                inProcess.adopt(reference);

                // Second in-process instance, same input. This is the control:
                // a plugin that randomises per instance (analog-modelled
                // components, per-instance seeds) differs against itself too,
                // and then a child difference is not a transport fault.
                auto referenceTwo = PluginHost::loadFromFile(nullTestFile, rate, blockSize);
                if (referenceTwo.error.isNotEmpty() || referenceTwo.instance == nullptr) {
                    fail("failed - second in-process load: "
                         + (referenceTwo.error.isNotEmpty() ? referenceTwo.error
                                                            : juce::String("no instance")));
                }
                PluginHost inProcessTwo;
                inProcessTwo.prepareToPlay(rate, blockSize);
                inProcessTwo.adopt(referenceTwo);

                // Child host.
                PluginHost child;
                child.prepareToPlay(rate, blockSize);
                juce::String childError;
                if (!child.loadChildHost(nullTestFile, 0, {}, rate, blockSize, 20000, childError)) {
                    fail("failed - child load: " + childError);
                }

                juce::AudioBuffer<float> referenceOutput(2, blockSize * blocks);
                juce::AudioBuffer<float> referenceTwoOutput(2, blockSize * blocks);
                juce::AudioBuffer<float> childOutput(2, blockSize * blocks);
                double phase = 0.0;
                const auto input = [&makeBlock, &phase, blocks, blockSize] {
                    juce::AudioBuffer<float> all(2, blockSize * blocks);
                    for (int i = 0; i < blocks; ++i) {
                        auto block = makeBlock(i, phase);
                        for (int channel = 0; channel < 2; ++channel) {
                            all.copyFrom(channel, i * blockSize, block, channel, 0, blockSize);
                        }
                    }
                    return all;
                }();

                const auto renderThrough = [&input, blocks, blockSize](PluginHost& host,
                                                            juce::AudioBuffer<float>& output) {
                    for (int i = 0; i < blocks; ++i) {
                        juce::AudioBuffer<float> block(2, blockSize);
                        for (int channel = 0; channel < 2; ++channel) {
                            block.copyFrom(channel, 0, input, channel, i * blockSize, blockSize);
                        }
                        host.processBlock(block);
                        for (int channel = 0; channel < 2; ++channel) {
                            output.copyFrom(channel, i * blockSize, block, channel, 0, blockSize);
                        }
                    }
                };

                renderThrough(inProcess, referenceOutput);
                renderThrough(inProcessTwo, referenceTwoOutput);
                renderThrough(child, childOutput);

                const auto compare = [blocks, blockSize](const juce::AudioBuffer<float>& a,
                                              const juce::AudioBuffer<float>& b) {
                    auto result = std::make_pair(0.0f, 0);
                    for (int channel = 0; channel < 2; ++channel) {
                        const auto* left = a.getReadPointer(channel);
                        const auto* right = b.getReadPointer(channel);
                        for (int i = 0; i < blocks * blockSize; ++i) {
                            if (left[i] != right[i]) ++result.second;
                            result.first = juce::jmax(result.first, std::abs(left[i] - right[i]));
                        }
                    }
                    return result;
                };

                const auto [baselineDiff, baselineCount] =
                    compare(referenceOutput, referenceTwoOutput);
                const auto [maxDifference, differingSamples] =
                    compare(referenceOutput, childOutput);

                const auto summary = juce::String("in-process peak ")
                                     + juce::String(referenceOutput.getMagnitude(0, 0,
                                                                                 blocks * blockSize),
                                                    6)
                                     + ", child peak "
                                     + juce::String(childOutput.getMagnitude(0, 0, blocks * blockSize),
                                                    6)
                                     + ", max diff " + juce::String(maxDifference, 8) + ", "
                                     + juce::String(differingSamples) + " differing samples"
                                     + "; baseline (in-process vs in-process) max diff "
                                     + juce::String(baselineDiff, 8) + ", "
                                     + juce::String(baselineCount) + " differing samples";

                // The plugin sets its own floor: compare the child against the
                // in-process instance, but only call it a transport fault when
                // it differs by more than the plugin differs from itself.
                const bool identical = differingSamples == 0;
                const bool withinTolerance = maxDifference <= juce::jmax(1.0e-6f, baselineDiff);
                const auto result = identical ? juce::String("identical - ") + summary
                                    : withinTolerance
                                        ? juce::String("within tolerance - ") + summary
                                        : juce::String("FAILED - ") + summary;
                juce::Logger::writeToLog("test-child-null: " + result);
                std::cout << result.toStdString() << "\n";
                std::cout.flush();
                ::exit(identical || withinTolerance ? 0 : 1);
            });
            return;
        }

        // Child-host smoke test (fault containment phase 1): loads one VST3 in
        // the child process only, renders through the shared audio channel and
        // reports whether output is non-silent. With --kill-after-ms N it then
        // kills the child mid-render and checks the insert plays DRY (the block
        // comes back bit-identical to its input) while this process stays alive.
        const int childTestIndex = args.indexOf("--test-child-host");
        if (childTestIndex >= 0 && childTestIndex + 1 < args.size()) {
            const juce::File childTestFile(args[childTestIndex + 1].unquoted());
            const int killIndex = args.indexOf("--kill-after-ms");
            const int killAfterMs = (killIndex >= 0 && killIndex + 1 < args.size())
                                        ? args[killIndex + 1].unquoted().getIntValue()
                                        : 0;
            juce::Thread::launch([childTestFile, killAfterMs] {
                const auto finish = [](const juce::String& result) {
                    juce::Logger::writeToLog("test-child-host: " + result);
                    std::cout << result.toStdString() << "\n";
                    std::cout.flush();
                    juce::MessageManager::callAsync([] {
                        if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
                    });
                };

                constexpr double rate = 48000.0;
                constexpr int blockSize = 256;
                PluginHost host;
                host.prepareToPlay(rate, blockSize);

                juce::String error;
                if (!host.loadChildHost(childTestFile, 0, {}, rate, blockSize, 20000, error)) {
                    finish("failed - " + error);
                    return;
                }

                // Render a sine and remember what went in, so a killed child can
                // be checked against true dry audio rather than "not silent".
                const auto renderBlock = [&host](double phaseStep, double& phase) {
                    juce::AudioBuffer<float> block(2, blockSize);
                    for (int i = 0; i < blockSize; ++i) {
                        const auto sample =
                            static_cast<float>(0.25 * std::sin(phase));
                        block.setSample(0, i, sample);
                        block.setSample(1, i, sample);
                        phase += phaseStep;
                    }
                    return block;
                };

                double phase = 0.0;
                const double step = 2.0 * juce::MathConstants<double>::pi * 220.0 / rate;
                float peak = 0.0f;
                for (int i = 0; i < 200; ++i) {
                    auto block = renderBlock(step, phase);
                    host.processBlock(block);
                    peak = juce::jmax(peak, block.getMagnitude(0, 0, blockSize));
                }
                if (peak <= 0.0f) {
                    finish("failed - the child rendered silence");
                    return;
                }

                if (killAfterMs <= 0) {
                    finish("rendered " + juce::String(200) + " blocks through the child, peak "
                           + juce::String(peak, 4) + ", status "
                           + (host.childHostStatus().failed ? "failed" : "ok"));
                    return;
                }

                juce::Thread::sleep(killAfterMs);
                const int pid = host.childProcessId();
                if (pid == 0) {
                    finish("failed - no child process to kill");
                    return;
                }
#if JUCE_WINDOWS
                const HANDLE victim = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
                if (victim != nullptr) {
                    ::TerminateProcess(victim, 0xC0000005);
                    ::CloseHandle(victim);
                }
#else
                juce::ignoreUnused(pid);
#endif
                juce::Thread::sleep(500);

                // The slot must now be dry: processBlock leaves the buffer alone.
                auto block = renderBlock(step, phase);
                const auto before = block;
                host.processBlock(block);
                bool dry = true;
                for (int channel = 0; channel < 2 && dry; ++channel) {
                    dry = std::memcmp(before.getReadPointer(channel),
                                      block.getReadPointer(channel),
                                      sizeof(float) * static_cast<std::size_t>(blockSize)) == 0;
                }
                const auto status = host.childHostStatus();
                finish((dry ? "killed child: insert is dry, app alive, exit 0x"
                            : "killed child: insert is NOT dry, exit 0x")
                       + juce::String::toHexString(status.exitCode).toUpperCase()
                       + ", status " + (status.failed ? "failed" : "not failed"));
            });
            // No window, no devices: this diagnostic only exercises the child
            // through PluginProcessHost, so it must not open the audio rig.
            return;
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
        if (auto* settings = properties.getUserSettings())
            settings->saveIfNeeded();
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
