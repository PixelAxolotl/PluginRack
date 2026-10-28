#include "DeviceMenu.h"

#include <algorithm>

namespace {
juce::StringArray enumerateInputs() {
    juce::AudioDeviceManager probe;
    probe.initialise(1, 0, nullptr, true, {}, nullptr);
    juce::StringArray names;
    for (auto* type : probe.getAvailableDeviceTypes()) {
        for (const auto& n : type->getDeviceNames(true)) {
            if (!names.contains(n)) names.add(n);
        }
    }
    names.sort(true);
    return names;
}

juce::StringArray enumerateOutputs() {
    juce::AudioDeviceManager probe;
    probe.initialise(0, 1, nullptr, true, {}, nullptr);
    juce::StringArray names;
    for (auto* type : probe.getAvailableDeviceTypes()) {
        for (const auto& n : type->getDeviceNames(false)) {
            if (!names.contains(n)) names.add(n);
        }
    }
    names.sort(true);
    return names;
}

void buildMenu(juce::PopupMenu& menu, const juce::String& header,
               const juce::String& current, const juce::StringArray& devices,
               std::function<void(juce::String)> onChoose) {
    menu.addSectionHeader(header);
    menu.addItem("System Default", true, current == "System Default",
                 [onChoose] { onChoose("System Default"); });
    menu.addItem("None \xe2\x80\x94 silent", true, current == "None \xe2\x80\x94 silent",
                 [onChoose] { onChoose("None \xe2\x80\x94 silent"); });
    menu.addSeparator();
    for (const auto& d : devices) {
        menu.addItem(d, true, current == d, [onChoose, d] { onChoose(d); });
    }
}
}

namespace DeviceMenu {
void showInputMenu(juce::Component& target, const juce::String& current,
                   std::function<void(juce::String)> onChoose) {
    juce::PopupMenu menu;
    buildMenu(menu, "INPUT DEVICES", current, enumerateInputs(), std::move(onChoose));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target).withMinimumWidth(240));
}

void showOutputMenu(juce::Component& target, const juce::String& current,
                    std::function<void(juce::String)> onChoose) {
    juce::PopupMenu menu;
    buildMenu(menu, "OUTPUT DEVICES", current, enumerateOutputs(), std::move(onChoose));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target).withMinimumWidth(240));
}
}
