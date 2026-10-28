#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

// Counts heap allocation events in the including translation unit.
// Tests include this header directly, which replaces the global
// operator new/delete for that TU with counting versions.
// The counter only goes up: frees never decrement it, so reset it right
// before the measured section and assert allocations() == 0 afterwards.
// Nothrow and aligned overloads are provided so any TU including this
// header keeps building; all of them count exactly one event per allocation.
struct AllocCounter {
    static long long& counter() {
        thread_local long long count = 0;
        return count;
    }

    static void reset() { counter() = 0; }
    static long long allocations() { return counter(); }
};

inline void* allocCounting(std::size_t size) {
    AllocCounter::counter() += 1;
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

inline void* allocCountingNothrow(std::size_t size) noexcept {
    AllocCounter::counter() += 1;
    return std::malloc(size == 0 ? 1 : size);
}

inline void* allocAlignedCounting(std::size_t size, std::size_t alignment) {
    AllocCounter::counter() += 1;
    if (size == 0) {
        size = 1;
    }
    if (alignment <= alignof(std::max_align_t)) {
        void* ptr = std::malloc(size);
        if (ptr == nullptr) {
            throw std::bad_alloc();
        }
        return ptr;
    }
    const std::size_t extra = alignment - 1 + sizeof(void*);
    void* raw = std::malloc(size + extra);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    const std::uintptr_t aligned =
        (reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*) + alignment - 1) &
        ~(alignment - 1);
    *(reinterpret_cast<void**>(aligned) - 1) = raw;
    return reinterpret_cast<void*>(aligned);
}

inline void* allocAlignedCountingNothrow(std::size_t size,
                                         std::size_t alignment) noexcept {
    AllocCounter::counter() += 1;
    if (size == 0) {
        size = 1;
    }
    if (alignment <= alignof(std::max_align_t)) {
        return std::malloc(size);
    }
    const std::size_t extra = alignment - 1 + sizeof(void*);
    void* raw = std::malloc(size + extra);
    if (raw == nullptr) {
        return nullptr;
    }
    const std::uintptr_t aligned =
        (reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*) + alignment - 1) &
        ~(alignment - 1);
    *(reinterpret_cast<void**>(aligned) - 1) = raw;
    return reinterpret_cast<void*>(aligned);
}

inline void freeCounting(void* ptr) noexcept {
    std::free(ptr);
}

inline void freeAlignedCounting(void* ptr, std::size_t alignment) noexcept {
    if (ptr == nullptr) {
        return;
    }
    if (alignment <= alignof(std::max_align_t)) {
        std::free(ptr);
        return;
    }
    std::free(*(reinterpret_cast<void**>(ptr) - 1));
}

void* operator new(std::size_t size) { return allocCounting(size); }
void* operator new[](std::size_t size) { return allocCounting(size); }
void operator delete(void* ptr) noexcept { freeCounting(ptr); }
void operator delete[](void* ptr) noexcept { freeCounting(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { freeCounting(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { freeCounting(ptr); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return allocCountingNothrow(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return allocCountingNothrow(size);
}
void operator delete(void* ptr, const std::nothrow_t&) noexcept { freeCounting(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { freeCounting(ptr); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocAlignedCounting(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocAlignedCounting(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* ptr, std::align_val_t alignment) noexcept {
    freeAlignedCounting(ptr, static_cast<std::size_t>(alignment));
}
void operator delete[](void* ptr, std::align_val_t alignment) noexcept {
    freeAlignedCounting(ptr, static_cast<std::size_t>(alignment));
}
void operator delete(void* ptr, std::size_t, std::align_val_t alignment) noexcept {
    freeAlignedCounting(ptr, static_cast<std::size_t>(alignment));
}
void operator delete[](void* ptr, std::size_t, std::align_val_t alignment) noexcept {
    freeAlignedCounting(ptr, static_cast<std::size_t>(alignment));
}
