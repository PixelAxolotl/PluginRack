#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>

// Counts outstanding heap allocations in the including translation unit.
// Tests include this header directly, which replaces the global
// operator new/delete for that TU with counting versions.
struct AllocCounter {
    static long long& counter() {
        thread_local long long count = 0;
        return count;
    }

    static void reset() { counter() = 0; }
    static long long count() { return counter(); }
};

inline void* allocCounting(std::size_t size) {
    AllocCounter::counter() += 1;
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

inline void freeCounting(void* ptr) noexcept {
    if (ptr != nullptr) {
        AllocCounter::counter() -= 1;
        std::free(ptr);
    }
}

void* operator new(std::size_t size) { return allocCounting(size); }
void* operator new[](std::size_t size) { return allocCounting(size); }
void operator delete(void* ptr) noexcept { freeCounting(ptr); }
void operator delete[](void* ptr) noexcept { freeCounting(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { freeCounting(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { freeCounting(ptr); }
