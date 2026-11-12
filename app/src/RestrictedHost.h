#pragma once

#include <JuceHeader.h>

// Child-process entry point for a hosted VST3 (fault containment, phase 1).
//
// This is the whole child side: main() calls runHost() for --plugin-host, the
// plugin loads here, and it renders every block the app hands over until the
// process dies. Nothing in this class touches an audio device, by design: ASIO
// is single client on the reference rig, so only the app opens devices and the
// child renders the buffers it is handed.
//
// The app-side half lives in PluginProcessHost (channels, handshake, watcher,
// control requests); the shared payload layouts are in PluginIpcLayout.h and
// PluginHostIpc.h. Keeping the two halves in separate files is deliberate:
// only this file runs plugin code, so only this file can take the app down.
//
// Runs with the user's token. The AppContainer profile is created by the app as
// preparation only; process isolation (a faulting plugin kills this process,
// not the app) is the benefit today, not sandboxing.
class RestrictedHost {
public:
    // Runs the plugin for the whole process lifetime; never returns.
    // sharedMappingName comes from the app: recomputing it from the plugin path
    // on both sides is fragile, and a mismatch made the host bail out with
    // "block == nullptr" after loading the plugin.
    static int runHost(const juce::File& file, int pluginUniqueId = 0,
                       const juce::String& stateBase64 = {},
                       const juce::String& sharedMappingName = {}, double sampleRate = 48000.0,
                       int blockSize = 256);

private:
    RestrictedHost() = delete;
};
