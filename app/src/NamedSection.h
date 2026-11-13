#pragma once

// Named shared-memory section for the plugin host audio block.
//
// Windows keeps a pagefile-backed section's *name* resolvable only while a
// handle to it is open. A mapped view alone is not enough: after the last
// handle closes, OpenFileMapping by name fails with ERROR_FILE_NOT_FOUND even
// though the view is still mapped and usable. The owner must therefore hold
// the create handle for as long as another process needs to open the name.
//
// This is deliberately free of JUCE so it can be covered by the core test
// binary; RestrictedHost owns the app-side instance and runHost owns the
// host-side one.

#include <cstddef>

#ifdef _WIN32
// See NamedEvent.h: keep the min/max macros out of headers that are included
// next to core code using std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pluginHostIpc {

class NamedSection {
public:
    NamedSection() = default;
    ~NamedSection() { close(); }

    NamedSection(const NamedSection&) = delete;
    NamedSection& operator=(const NamedSection&) = delete;

    // Creates and maps the section. The caller must keep this object alive
    // for as long as the name must remain openable by the host.
    bool create(const wchar_t* name, std::size_t bytes) {
        close();
        mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                       PAGE_READWRITE | SEC_COMMIT, 0,
                                       static_cast<DWORD>(bytes), name);
        lastError = ::GetLastError();
        if (mapping == nullptr) return false;

        view = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
        lastError = ::GetLastError();
        if (view == nullptr) {
            ::CloseHandle(mapping);
            mapping = nullptr;
            return false;
        }
        sectionSize = bytes;
        // Keep `mapping` open: the name is only resolvable while a handle to
        // the section exists, even though the view already references it.
        return true;
    }

    // Opens and maps an existing section by name. The opened handle is kept
    // open so the name cannot vanish underneath a second opener.
    bool open(const wchar_t* name, std::size_t bytes) {
        close();
        mapping = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        lastError = ::GetLastError();
        if (mapping == nullptr) return false;

        view = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
        lastError = ::GetLastError();
        if (view == nullptr) {
            ::CloseHandle(mapping);
            mapping = nullptr;
            return false;
        }
        sectionSize = bytes;
        return true;
    }

    bool isOpen() const { return view != nullptr; }
    void* data() const { return view; }
    std::size_t size() const { return sectionSize; }
    unsigned long error() const { return lastError; }

    void close() {
        if (view != nullptr) {
            ::UnmapViewOfFile(view);
            view = nullptr;
        }
        if (mapping != nullptr) {
            ::CloseHandle(mapping);
            mapping = nullptr;
        }
        sectionSize = 0;
    }

private:
    HANDLE mapping = nullptr;
    void* view = nullptr;
    std::size_t sectionSize = 0;
    unsigned long lastError = 0;
};

} // namespace pluginHostIpc

#endif // _WIN32
