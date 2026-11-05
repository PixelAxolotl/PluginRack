#pragma once

#include <JuceHeader.h>

#if JUCE_WINDOWS
#include <windows.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#endif

namespace realtime {

// Registers the calling thread with MMCSS ("Pro Audio"). Audio device callbacks
// run on driver threads the OS does not prioritise for real-time work, so the
// scheduler can preempt them mid-block; MMCSS gives them the same protection a
// DAW asks for. Registration is per thread, once, and undone at thread exit.
//
// This is deliberately not a scoped per-callback object: AvSetMmThreadCharacteristics
// is a kernel call, and the clock callback can run hundreds of times a second.
inline void registerProAudioThread() {
#if JUCE_WINDOWS
    struct Registration {
        HANDLE handle = nullptr;
        Registration() {
            DWORD taskIndex = 0;
            handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        }
        ~Registration() {
            if (handle != nullptr) AvRevertMmThreadCharacteristics(handle);
        }
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
    };
    thread_local Registration registration;
    juce::ignoreUnused(registration);
#endif
}

} // namespace realtime
