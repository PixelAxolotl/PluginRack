#pragma once

// Named auto-reset event for waking the plugin host's audio loop.
//
// The host used to poll its shared audio block with Thread::sleep(1). That
// sleep is a 1 ms timeout, so it is bounded by the system timer tick. Windows
// 11 ignores a background/occluded process's timer-resolution request, which
// drops that tick to 15.6 ms and makes every host block miss the audio
// callback's budget. A signalled event wakes the waiter immediately and does
// not depend on the timer tick at all.
//
// Like NamedSection, this is deliberately free of JUCE so it can be covered by
// the core test binary.

#include <cstddef>

#ifdef _WIN32
#include <windows.h>

namespace pluginHostIpc {

class NamedEvent {
public:
    NamedEvent() = default;
    ~NamedEvent() { close(); }

    NamedEvent(const NamedEvent&) = delete;
    NamedEvent& operator=(const NamedEvent&) = delete;

    // Creates the event (or opens it when the name already exists) and starts
    // it unsignalled. The caller must keep this object alive for as long as
    // the host needs to open the name.
    bool create(const wchar_t* name) {
        close();
        event = ::CreateEventW(nullptr, FALSE, FALSE, name);
        lastError = ::GetLastError();
        if (event == nullptr) return false;
        ::ResetEvent(event);
        return true;
    }

    // Opens an existing event by name. The host both waits (SYNCHRONIZE) and
    // signals (EVENT_MODIFY_STATE) on these handles.
    bool open(const wchar_t* name) {
        close();
        event = ::OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
        lastError = ::GetLastError();
        return event != nullptr;
    }

    bool isOpen() const { return event != nullptr; }

    bool signal() const { return event != nullptr && ::SetEvent(event) != FALSE; }
    bool reset() const { return event != nullptr && ::ResetEvent(event) != FALSE; }

    // Waits for a signal for up to timeoutMs. Returns true when the wait was
    // satisfied (auto-reset then clears the signal), false on timeout or when
    // this object does not own a valid handle. A signal wakes the waiter
    // immediately; it is never delayed to the next system timer tick.
    bool wait(unsigned long timeoutMs) const {
        return event != nullptr && ::WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;
    }

    unsigned long error() const { return lastError; }

    void close() {
        if (event != nullptr) {
            ::CloseHandle(event);
            event = nullptr;
        }
    }

private:
    HANDLE event = nullptr;
    unsigned long lastError = 0;
};

} // namespace pluginHostIpc

#endif // _WIN32
