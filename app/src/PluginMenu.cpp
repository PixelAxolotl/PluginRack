#include "PluginMenu.h"

#include "PluginLibrary.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace {

// One chooser at a time: the menu only ever opens one, and JUCE wants the
// chooser to outlive its async callback.
std::unique_ptr<juce::FileChooser> activeChooser;

struct Entry {
    juce::String label;
    juce::File file;
};

struct Vendor {
    juce::String name;
    std::vector<Entry> entries;
};

bool caseInsensitiveLess(const juce::String& a, const juce::String& b) {
    return a.compareIgnoreCase(b) < 0;
}

void addFormatBranch(juce::PopupMenu& menu, const juce::String& format,
                     const juce::Array<juce::PluginDescription>& plugins,
                     const std::function<void(const juce::File&)>& onChoose) {
    std::vector<Vendor> vendors;
    for (const auto& description : plugins) {
        const auto vendorName = PluginLibrary::vendorNameFor(description);
        auto vendor = std::find_if(vendors.begin(), vendors.end(),
                                   [&vendorName](const Vendor& candidate) {
                                       return candidate.name.equalsIgnoreCase(vendorName);
                                   });
        if (vendor == vendors.end()) {
            vendors.push_back({vendorName, {}});
            vendor = std::prev(vendors.end());
        }
        vendor->entries.push_back(
            {PluginLibrary::displayNameFor(description),
             juce::File(description.fileOrIdentifier)});
    }

    std::sort(vendors.begin(), vendors.end(), [](const Vendor& a, const Vendor& b) {
        return caseInsensitiveLess(a.name, b.name);
    });

    juce::PopupMenu formatMenu;
    for (auto& vendor : vendors) {
        std::sort(vendor.entries.begin(), vendor.entries.end(),
                  [](const Entry& a, const Entry& b) {
                      if (a.label.compareIgnoreCase(b.label) != 0)
                          return caseInsensitiveLess(a.label, b.label);
                      return caseInsensitiveLess(a.file.getFullPathName(),
                                                 b.file.getFullPathName());
                  });

        juce::PopupMenu vendorMenu;
        juce::StringArray usedLabels;
        for (const auto& entry : vendor.entries) {
            // Two builds of the same plugin must stay tellable apart.
            auto label = entry.label;
            for (int suffix = 2; usedLabels.contains(label); ++suffix)
                label = entry.label + " (" + juce::String(suffix) + ")";
            usedLabels.add(label);
            vendorMenu.addItem(label, true, false, [onChoose, file = entry.file] {
                onChoose(file);
            });
        }
        formatMenu.addSubMenu(vendor.name, vendorMenu);
    }

    menu.addSubMenu(format, formatMenu);
}

void addPluginBranches(juce::PopupMenu& menu,
                       const juce::Array<juce::PluginDescription>& plugins,
                       const std::function<void(const juce::File&)>& onChoose) {
    juce::StringArray formats;
    for (const auto& description : plugins)
        formats.addIfNotAlreadyThere(description.pluginFormatName.isNotEmpty()
                                         ? description.pluginFormatName
                                         : juce::String("Plugins"));

    if (formats.isEmpty()) {
        menu.addItem("No plugins found", false, false, nullptr);
        return;
    }

    formats.sort(true);
    for (const auto& format : formats) {
        juce::Array<juce::PluginDescription> ofFormat;
        for (const auto& description : plugins)
            if ((description.pluginFormatName.isNotEmpty()
                     ? description.pluginFormatName
                     : juce::String("Plugins")) == format)
                ofFormat.add(description);
        addFormatBranch(menu, format, ofFormat, onChoose);
    }
}

void reopenMenu(juce::Component::SafePointer<juce::Component> anchor,
                std::function<void(const juce::File&)> onChoose) {
    juce::MessageManager::callAsync([anchor, onChoose = std::move(onChoose)]() mutable {
        if (anchor == nullptr) return;
        PluginMenu::showAddPluginMenu(*anchor, std::move(onChoose));
    });
}

void addPluginFolder(juce::Component::SafePointer<juce::Component> anchor,
                     std::function<void(const juce::File&)> onChoose) {
    activeChooser = std::make_unique<juce::FileChooser>(
        "Add a plugin folder", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        juce::String());
    activeChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [anchor, onChoose](const juce::FileChooser& chooser) {
            const auto folder = chooser.getResult();
            if (folder == juce::File() || !folder.isDirectory()) return;
            auto& library = PluginLibrary::getInstance();
            library.addExtraFolder(folder);
            library.rescan(folder);
            reopenMenu(anchor, std::move(onChoose));
        });
}

void browseForFile(std::function<void(const juce::File&)> onChoose) {
    activeChooser = std::make_unique<juce::FileChooser>(
        "Select a VST3 plugin", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.vst3");
    activeChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [onChoose = std::move(onChoose)](const juce::FileChooser& chooser) {
            const auto file = chooser.getResult();
            if (file == juce::File()) return;
            onChoose(file);
        });
}

// The rows that follow the plugin list: what was found, then the folder
// actions, then the file-browser fallback.
void addActionRows(juce::PopupMenu& menu, juce::Component::SafePointer<juce::Component> anchor,
                   const std::function<void(const juce::File&)>& onChoose) {
    auto& library = PluginLibrary::getInstance();
    menu.addSeparator();
    menu.addItem(library.getStatusText(), false, false, nullptr);
    menu.addItem("Add plugin folder...", true, false, [anchor, onChoose] {
        addPluginFolder(anchor, onChoose);
    });
    menu.addItem("Rescan plugin folders", true, false, [anchor, onChoose] {
        PluginLibrary::getInstance().rescan();
        reopenMenu(anchor, onChoose);
    });
    menu.addItem("Browse...", true, false, [onChoose] { browseForFile(onChoose); });
}

void buildAddPluginMenu(juce::PopupMenu& menu,
                        juce::Component::SafePointer<juce::Component> anchor,
                        const std::function<void(const juce::File&)>& onChoose) {
    menu.addSectionHeader("ADD PLUGIN");
    addPluginBranches(menu, PluginLibrary::getInstance().getPlugins(), onChoose);
    addActionRows(menu, anchor, onChoose);
}

} // namespace

namespace PluginMenu {

void showAddPluginMenu(juce::Component& anchor,
                       std::function<void(const juce::File&)> onChoose) {
    auto& library = PluginLibrary::getInstance();
    // First click after launch: the cache is loaded, but a library that was
    // never scanned still needs one so the menu has something to show.
    if (!library.hasScanned() && !library.isScanning()) library.beginBackgroundScan();

    const juce::Component::SafePointer<juce::Component> safeAnchor(&anchor);

    juce::PopupMenu menu;
    buildAddPluginMenu(menu, safeAnchor, onChoose);

    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(anchor)
                           .withMinimumWidth(260));
}

} // namespace PluginMenu
