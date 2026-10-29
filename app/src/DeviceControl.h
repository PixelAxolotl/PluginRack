#pragma once

#include <JuceHeader.h>

#include <functional>

#include "Theme.h"

class DeviceControl final : public juce::Component, public juce::SettableTooltipClient {
public:
    DeviceControl();

    void setTitle(const juce::String& t) { title = t; updateTooltip(); repaint(); }
    void setDeviceName(const juce::String& n) { deviceName = n; updateTooltip(); repaint(); }
    void setError(const juce::String& e) { error = e; updateTooltip(); repaint(); }
    void setOnClick(std::function<void()> cb) { onClick = std::move(cb); }

    juce::String getTitle() const { return title; }
    juce::String getDeviceName() const { return deviceName; }
    juce::String getError() const { return error; }

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered = false; pressed = false; repaint(); }
    void mouseUp(const juce::MouseEvent&) override { pressed = false; repaint(); }
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void updateTooltip();

    juce::String title;
    juce::String deviceName;
    juce::String error;
    std::function<void()> onClick;
    bool hovered = false;
    bool pressed = false;
};
