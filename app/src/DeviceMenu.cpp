#include "DeviceMenu.h"

#include "DeviceNames.h"

namespace {
const juce::String kSystemDefault = systemDefaultDeviceName();
const juce::String kNone = silentDeviceName();

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
#if JUCE_WINDOWS && !JUCE_ASIO
    // ASIO is a compile-time JUCE flag; without it the type can never appear
    // in the list (DeviceStream.cpp only tries ASIO when JUCE_ASIO is set).
    menu.addSeparator();
    menu.addItem(juce::String("ASIO (SDK not installed)"), false, false, [] {});
#endif
}
}

namespace DeviceMenu {
void showDeviceMenu(bool forInput, juce::Component& target, const juce::String& current,
                    const juce::StringArray& devices,
                    std::function<void(juce::String)> onChoose) {
    juce::PopupMenu menu;
    buildMenu(menu, forInput ? "INPUT DEVICES" : "OUTPUT DEVICES", current, devices,
              std::move(onChoose));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target).withMinimumWidth(240));
}
}
