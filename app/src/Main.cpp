#include <JuceHeader.h>

#include "ConsoleLookAndFeel.h"
#include "DeviceStream.h"
#include "MainComponent.h"

#include <iostream>

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow()
        : juce::DocumentWindow("PluginRack", juce::Colours::darkgrey, allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(), true);
        setResizeLimits(1180, 700, 10000, 10000);
        centreWithSize(1600, 900);
        setVisible(true);
    }

    void closeButtonPressed() override {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class PluginRackApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "PluginRack"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override {
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
        mainWindow = std::make_unique<MainWindow>();
    }

    void shutdown() override {
        if (mainWindow != nullptr) {
            if (auto* mainComponent = dynamic_cast<MainComponent*>(mainWindow->getContentComponent()))
                mainComponent->saveSceneOnExit();
        }
        mainWindow.reset();
        tooltipWindow.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        lookAndFeel.reset();
    }

private:
    std::unique_ptr<ConsoleLookAndFeel> lookAndFeel;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(PluginRackApplication)
