#include <JuceHeader.h>

#include "ConsoleLookAndFeel.h"
#include "DeviceStream.h"
#include "MainComponent.h"

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
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override {
        juce::SystemStats::setApplicationCrashHandler(&writeCrashReport);
        pluginRackLogDirectory().createDirectory();
        fileLogger = std::make_unique<juce::FileLogger>(
            pluginRackLogDirectory().getChildFile("PluginRack.log"), "PluginRack started");
        juce::Logger::setCurrentLogger(fileLogger.get());
        juce::Logger::writeToLog("initialise: " + commandLine);

        juce::StringArray args;
        args.addTokens(commandLine, true);

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
    }

    void shutdown() override {
        juce::Logger::writeToLog("shutdown");
        if (mainWindow != nullptr) {
            if (auto* mainComponent = dynamic_cast<MainComponent*>(mainWindow->getContentComponent()))
                mainComponent->saveSceneOnExit();
        }
        mainWindow.reset();
        tooltipWindow.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        lookAndFeel.reset();
        juce::Logger::setCurrentLogger(nullptr);
        fileLogger.reset();
    }

private:
    std::unique_ptr<juce::FileLogger> fileLogger;
    juce::ApplicationProperties properties;
    std::unique_ptr<ConsoleLookAndFeel> lookAndFeel;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(PluginRackApplication)
