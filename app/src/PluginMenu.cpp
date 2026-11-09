#include "PluginMenu.h"

#include "PluginLibrary.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace {

// One chooser at a time: the menu only ever opens one, and JUCE wants the
// chooser to outlive its async callback.
std::unique_ptr<juce::FileChooser> activeChooser;

using ChooseCallback = std::function<void(const juce::File&, int uniqueId)>;

struct Entry {
    juce::String label;
    juce::File file;
    // Which class inside the bundle this row stands for. A bundle can expose
    // several processors, so the path alone does not identify the plugin.
    int uniqueId = 0;
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
                     const ChooseCallback& onChoose) {
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
             juce::File(description.fileOrIdentifier), description.uniqueId});
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
            vendorMenu.addItem(label, true, false,
                               [onChoose, file = entry.file, uniqueId = entry.uniqueId] {
                                   onChoose(file, uniqueId);
                               });
        }
        formatMenu.addSubMenu(vendor.name, vendorMenu);
    }

    menu.addSubMenu(format, formatMenu);
}

void addPluginBranches(juce::PopupMenu& menu,
                       const juce::Array<juce::PluginDescription>& plugins,
                       const ChooseCallback& onChoose) {
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

void reopenMenu(juce::Component::SafePointer<juce::Component> anchor, ChooseCallback onChoose,
                PluginMenu::BuiltInCallback onBuiltIn = {}) {
    juce::MessageManager::callAsync(
        [anchor, onChoose = std::move(onChoose), onBuiltIn = std::move(onBuiltIn)]() mutable {
            if (anchor == nullptr) return;
            if (onBuiltIn)
                PluginMenu::showAddPluginMenu(*anchor, std::move(onChoose), std::move(onBuiltIn));
            else
                PluginMenu::showAddPluginMenu(*anchor, std::move(onChoose));
        });
}

// Adding a folder starts a background scan, so a menu reopened right away can
// only show the catalogue from before it. This listener reopens the picker
// once that scan finishes, which is when the new folder's plugins exist.
// PluginLibrary clears `scanning` and broadcasts on the message thread, so a
// plain ChangeListener is enough.
class ScanRefresh final : public juce::ChangeListener {
public:
    static ScanRefresh& getInstance() {
        // Deliberately leaked: it has to outlive the shutdown-time teardown of
        // the library it listens to, and it is idle between scans.
        static auto* instance = new ScanRefresh();
        return *instance;
    }

    void refreshWhenScanFinishes(juce::Component::SafePointer<juce::Component> anchor,
                                 ChooseCallback onChoose,
                                 PluginMenu::BuiltInCallback onBuiltIn = {}) {
        pendingAnchor = anchor;
        pendingChoose = std::move(onChoose);
        pendingBuiltIn = std::move(onBuiltIn);
        if (!listening) {
            PluginLibrary::getInstance().addChangeListener(this);
            listening = true;
        }
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        if (pendingAnchor == nullptr || !pendingAnchor->isShowing()) {
            clearPending();
            return;
        }
        if (PluginLibrary::getInstance().isScanning()) return;
        auto anchor = pendingAnchor;
        auto onChoose = std::move(pendingChoose);
        auto onBuiltIn = std::move(pendingBuiltIn);
        clearPending();
        if (onBuiltIn)
            PluginMenu::showAddPluginMenu(*anchor, std::move(onChoose), std::move(onBuiltIn));
        else
            PluginMenu::showAddPluginMenu(*anchor, std::move(onChoose));
    }

    void clearPending() {
        pendingAnchor = nullptr;
        pendingChoose = nullptr;
        pendingBuiltIn = nullptr;
        if (listening) {
            PluginLibrary::getInstance().removeChangeListener(this);
            listening = false;
        }
    }

    juce::Component::SafePointer<juce::Component> pendingAnchor;
    ChooseCallback pendingChoose;
    PluginMenu::BuiltInCallback pendingBuiltIn;
    bool listening = false;
};

void addPluginFolder(juce::Component::SafePointer<juce::Component> anchor, ChooseCallback onChoose,
                     PluginMenu::BuiltInCallback onBuiltIn = {}) {
    activeChooser = std::make_unique<juce::FileChooser>(
        "Add a plugin folder", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        juce::String());
    activeChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [anchor, onChoose = std::move(onChoose), onBuiltIn = std::move(onBuiltIn)](
            const juce::FileChooser& chooser) mutable {
            const auto folder = chooser.getResult();
            if (folder == juce::File() || !folder.isDirectory()) return;
            auto& library = PluginLibrary::getInstance();
            library.addExtraFolder(folder);
            // Reopen now (the status row shows the scan running) and again when
            // it finishes, because only then does the menu list the new folder.
            ScanRefresh::getInstance().refreshWhenScanFinishes(anchor, onChoose, onBuiltIn);
            library.rescan(folder);
            reopenMenu(anchor, std::move(onChoose), std::move(onBuiltIn));
        });
}

void browseForFile(ChooseCallback onChoose) {
    activeChooser = std::make_unique<juce::FileChooser>(
        "Select a VST3 plugin", juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.vst3");
    activeChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [onChoose = std::move(onChoose)](const juce::FileChooser& chooser) mutable {
            const auto file = chooser.getResult();
            if (file == juce::File()) return;
            // Picked by path, so the class inside the bundle is unknown here:
            // 0 asks the loader for the bundle's first class, as before.
            onChoose(file, 0);
        });
}

// The rows that follow the plugin list: what was found, then the folder
// actions, then the file-browser fallback.
void addActionRows(juce::PopupMenu& menu, juce::Component::SafePointer<juce::Component> anchor,
                   const ChooseCallback& onChoose,
                   const PluginMenu::BuiltInCallback& onBuiltIn = {}) {
    auto& library = PluginLibrary::getInstance();
    menu.addSeparator();
    menu.addItem(library.getStatusText(), false, false, nullptr);
    menu.addItem("Add plugin folder...", true, false, [anchor, onChoose, onBuiltIn] {
        addPluginFolder(anchor, onChoose, onBuiltIn);
    });
    menu.addItem("Rescan plugin folders", true, false, [anchor, onChoose, onBuiltIn] {
        ScanRefresh::getInstance().refreshWhenScanFinishes(anchor, onChoose, onBuiltIn);
        PluginLibrary::getInstance().rescan();
        reopenMenu(anchor, onChoose, onBuiltIn);
    });
    menu.addItem("Browse...", true, false, [onChoose] { browseForFile(onChoose); });
}

void buildAddPluginMenu(juce::PopupMenu& menu,
                        juce::Component::SafePointer<juce::Component> anchor,
                        const ChooseCallback& onChoose) {
    menu.addSectionHeader("ADD PLUGIN");
    addPluginBranches(menu, PluginLibrary::getInstance().getPlugins(), onChoose);
    addActionRows(menu, anchor, onChoose);
}

void buildAddPluginMenu(juce::PopupMenu& menu,
                        juce::Component::SafePointer<juce::Component> anchor,
                        const ChooseCallback& onChoose,
                        const PluginMenu::BuiltInCallback& onBuiltIn) {
    menu.addSectionHeader("ADD PLUGIN");

    // Built-in effects sit above the VST branches so the singleton move is
    // discoverable without leaving the picker. Dynamics id 1 is the
    // compressor and id 2 the gate (the original chain order).
    juce::PopupMenu builtInMenu;
    builtInMenu.addItem("EQ", true, false, [onBuiltIn] {
        onBuiltIn({pluginrack::ChainItemType::Eq, 0});
    });
    // Compressor and gate are one object (the Dynamics card): one entry
    // restores or moves both rows together, each still switched on its own.
    builtInMenu.addItem("Comp/Gate", true, false, [onBuiltIn] {
        onBuiltIn({pluginrack::ChainItemType::Dynamics, 1});
    });
    builtInMenu.addItem("Multiband Comp", true, false, [onBuiltIn] {
        onBuiltIn({pluginrack::ChainItemType::Multiband, 1});
    });
    builtInMenu.addItem("De-esser", true, false, [onBuiltIn] {
        onBuiltIn({pluginrack::ChainItemType::DeEsser, 1});
    });
    builtInMenu.addItem("Limiter", true, false, [onBuiltIn] {
        onBuiltIn({pluginrack::ChainItemType::Limiter, 1});
    });
    menu.addSubMenu("Built-In effects", builtInMenu);

    addPluginBranches(menu, PluginLibrary::getInstance().getPlugins(), onChoose);
    addActionRows(menu, anchor, onChoose, onBuiltIn);
}

} // namespace

namespace PluginMenu {

void showAddPluginMenu(juce::Component& anchor, std::function<void(const juce::File&, int)> onChoose) {
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

void showAddPluginMenu(juce::Component& anchor, std::function<void(const juce::File&, int)> onChoose,
                       BuiltInCallback onBuiltIn) {
    auto& library = PluginLibrary::getInstance();
    if (!library.hasScanned() && !library.isScanning()) library.beginBackgroundScan();

    const juce::Component::SafePointer<juce::Component> safeAnchor(&anchor);

    juce::PopupMenu menu;
    buildAddPluginMenu(menu, safeAnchor, onChoose, onBuiltIn);

    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(anchor)
                           .withMinimumWidth(260));
}

} // namespace PluginMenu
