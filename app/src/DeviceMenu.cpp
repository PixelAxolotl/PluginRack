#include "DeviceMenu.h"

namespace {
const juce::String kSystemDefault = "System Default";
const juce::String kNone = "None - silent";

void buildMenu(juce::PopupMenu& menu, const juce::String& header,
               const juce::String& current, const juce::StringArray& devices,
               std::function<void(juce::String)> onChoose) {
    menu.addSectionHeader(header);
    menu.addItem(kSystemDefault, true, current == kSystemDefault,
                 [onChoose] { onChoose(kSystemDefault); });
    menu.addItem(kNone, true, current == kNone,
                 [onChoose] { onChoose(kNone); });
    menu.addSeparator();
    for (const auto& d : devices) {
        menu.addItem(d, true, current == d, [onChoose, d] { onChoose(d); });
    }
}
}

namespace DeviceMenu {
void showInputMenu(juce::Component& target, const juce::String& current,
                   const juce::StringArray& devices,
                   std::function<void(juce::String)> onChoose) {
    juce::PopupMenu menu;
    buildMenu(menu, "INPUT DEVICES", current, devices, std::move(onChoose));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target).withMinimumWidth(240));
}

void showOutputMenu(juce::Component& target, const juce::String& current,
                    const juce::StringArray& devices,
                    std::function<void(juce::String)> onChoose) {
    juce::PopupMenu menu;
    buildMenu(menu, "OUTPUT DEVICES", current, devices, std::move(onChoose));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target).withMinimumWidth(240));
}
}
