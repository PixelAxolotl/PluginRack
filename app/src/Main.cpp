#include <JuceHeader.h>

#include "ConsoleLookAndFeel.h"
#include "MainComponent.h"

#include <iostream>

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow()
        : juce::DocumentWindow("PluginRack", juce::Colours::darkgrey, allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(), true);
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
            juce::AudioDeviceManager probeIn, probeOut;
            probeIn.initialise(1, 0, nullptr, true, {}, nullptr);
            probeOut.initialise(0, 1, nullptr, true, {}, nullptr);

            std::cout << "Input devices:\n";
            juce::StringArray inNames;
            for (auto* type : probeIn.getAvailableDeviceTypes()) {
                for (const auto& n : type->getDeviceNames(true)) {
                    if (!inNames.contains(n)) inNames.add(n);
                }
            }
            inNames.sort(true);
            for (const auto& n : inNames) std::cout << "  " << n.toStdString() << "\n";
            if (inNames.isEmpty()) std::cout << "  (none)\n";

            std::cout << "Output devices:\n";
            juce::StringArray outNames;
            for (auto* type : probeOut.getAvailableDeviceTypes()) {
                for (const auto& n : type->getDeviceNames(false)) {
                    if (!outNames.contains(n)) outNames.add(n);
                }
            }
            outNames.sort(true);
            for (const auto& n : outNames) std::cout << "  " << n.toStdString() << "\n";
            if (outNames.isEmpty()) std::cout << "  (none)\n";

            quit();
            return;
        }

        int idx = args.indexOf("--screenshot");
        if (idx >= 0 && idx + 1 < args.size()) {
            const juce::File outPath(args[idx + 1]);
            outPath.getParentDirectory().createDirectory();
            lookAndFeel = std::make_unique<ConsoleLookAndFeel>();
            juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());
            auto* comp = new MainComponent();
            comp->setSize(1600, 900);
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
