#include <JuceHeader.h>

#include "MainComponent.h"

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow()
        : juce::DocumentWindow("PluginRack", juce::Colours::darkgrey, allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(), true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class PluginRackApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override {
        return "PluginRack";
    }

    const juce::String getApplicationVersion() override {
        return "0.1.0";
    }

    bool moreThanOneInstanceAllowed() override {
        return true;
    }

    void initialise(const juce::String&) override {
        mainWindow = std::make_unique<MainWindow>();
    }

    void shutdown() override {
        mainWindow.reset();
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(PluginRackApplication)
