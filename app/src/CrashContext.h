#pragma once

// Which insert was rendering when the process died.
//
// A crash inside a plugin names a module and an offset and nothing else, so the
// report could not say *which* plugin in the rack faulted - and that ambiguity
// cost real debugging time (a crash blamed on the wrong plugin). The audio path
// now leaves a label behind while it renders an insert, and the crash handler
// prints it.
//
// Why this shape:
//   - The audio thread must not allocate, so it only stores a pointer
//     (setCurrentRender/clearCurrentRender: one store each).
//   - The label text is built once, on the message thread, when an insert is
//     created (registerLabel).
//   - Labels are intentionally never freed: the audio thread may hold the
//     pointer, and a crash report can read it at any moment, including during
//     teardown. The registry is deduplicated by text, so reloading a scene does
//     not grow it.
//
// Header-only and JUCE-free on purpose, exactly like NamedSection.h and
// NamedEvent.h: the core test binary links no app code, so anything it must
// cover has to be reachable from a header. The `inline` functions give the
// program a single registry and a single thread-local slot.

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace crash_context {

namespace detail {

inline std::mutex& registryMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::vector<std::unique_ptr<std::string>>& registry() {
    static std::vector<std::unique_ptr<std::string>> labels;
    return labels;
}

// One pointer per thread: the audio callback sets it immediately before it
// hands the block to the plugin, so a fault in that call reports this insert.
inline const char*& currentSlot() {
    static thread_local const char* label = nullptr;
    return label;
}

} // namespace detail

// Registers (or returns the existing) stable label for an insert. Message
// thread only. The returned pointer stays valid for the life of the process.
inline const char* registerLabel(const std::string& text) {
    const std::lock_guard<std::mutex> lock(detail::registryMutex());
    for (const auto& existing : detail::registry()) {
        if (*existing == text) return existing->c_str();
    }
    detail::registry().push_back(std::make_unique<std::string>(text));
    return detail::registry().back()->c_str();
}

// Audio thread: publish or clear the label of the insert being rendered.
inline void setCurrentRender(const char* label) noexcept {
    detail::currentSlot() = label;
}

inline void clearCurrentRender() noexcept {
    detail::currentSlot() = nullptr;
}

// Crash handler: the label of the insert that was rendering on *this* thread,
// or an empty string.
inline std::string currentRender() {
    const char* label = detail::currentSlot();
    return label != nullptr ? std::string(label) : std::string();
}

// Test hook: how many distinct labels the registry holds.
inline std::size_t labelCount() {
    const std::lock_guard<std::mutex> lock(detail::registryMutex());
    return detail::registry().size();
}

} // namespace crash_context
