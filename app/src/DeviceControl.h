#pragma once

#include <JuceHeader.h>

#include <functional>

#include "Theme.h"

class DeviceControl final : public juce::Component, public juce::SettableTooltipClient {
public:
    DeviceControl();

    void setTitle(const juce::String& t) { title = t; updateTooltip(); updateTooltipForBus(); repaint(); }
    void setDeviceName(const juce::String& n) { deviceName = n; updateTooltip(); repaint(); }
    // Raw JUCE device type ("Windows Audio", "ASIO", ...) this stream actually
    // opened in, drawn as a small tag so the backend is visible per strip.
    void setBackendType(const juce::String& type) { backendType = type; updateTooltip(); repaint(); }
    void setError(const juce::String& e) { error = e; updateTooltip(); repaint(); }
    // Blue status dot on the output strip that currently drives the engine
    // clock, so the clock device is identifiable at a glance.
    void setClock(bool c) { if (isClock == c) return; isClock = c; updateTooltip(); repaint(); }
    void setOnClick(std::function<void()> cb) { onClick = std::move(cb); }
    void setOnRename(std::function<void()> cb) { onRename = std::move(cb); }
    // Bus strips have no device or clock: their tooltip names the gesture
    // that actually exists (right-click; double-click is a no-op below) and
    // skips the device line. Re-applied after every updateTooltip() because
    // setTitle() rebuilds the shared text.
    void setBusMode(bool b) { busMode = b; updateTooltipForBus(); repaint(); }

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered = false; pressed = false; repaint(); }
    void mouseUp(const juce::MouseEvent&) override { pressed = false; repaint(); }
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void updateTooltip();
    void updateTooltipForBus() {
        if (busMode) setTooltip(title + "\nsum of sends\nRight-click to rename this bus");
    }

    juce::String title;
    juce::String deviceName;
    juce::String backendType;
    juce::String error;
    bool isClock = false;
    std::function<void()> onClick;
    std::function<void()> onRename;
    bool hovered = false;
    bool pressed = false;
    bool busMode = false;
};
