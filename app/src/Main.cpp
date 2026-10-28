#include <JuceHeader.h>

#include "MainComponent.h"

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
        int idx = args.indexOf("--screenshot");
        if (idx >= 0 && idx + 1 < args.size()) {
            const juce::File outPath(args[idx + 1]);
            outPath.getParentDirectory().createDirectory();
            auto* comp = new MainComponent();
            comp->setSize(1600, 900);
            juce::Image snapshot = comp->createComponentSnapshot(comp->getLocalBounds(), false, 1.0f);
            juce::PNGImageFormat png;
            if (auto stream = outPath.createOutputStream()) {
                png.writeImageToStream(snapshot, *stream);
            }
            delete comp;
            quit();
            return;
        }
        mainWindow = std::make_unique<MainWindow>();
    }

    void shutdown() override {
        mainWindow.reset();
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(PluginRackApplication)