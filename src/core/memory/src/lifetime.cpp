// The process-lifetime registry, and its bridge to LeakSanitizer. Tasks 2.11 and 2.12.

#include <cy/core/memory/lifetime.h>

#include <atomic>
#include <mutex>

// The sanitizer's own header, present only in a build that has the runtime. __has_include rather
// than a compiler version test: the header ships with the compiler's resource directory, and its
// absence is exactly the condition that matters.
#if defined(__has_include)
#    if __has_include(<sanitizer/lsan_interface.h>)
#        define CY_HAS_LSAN_INTERFACE 1
#    endif
#endif

#if defined(CY_HAS_LSAN_INTERFACE)
#    include <sanitizer/lsan_interface.h>
#endif

// Whether the runtime is actually linked in. Clang states it through __has_feature; GCC defines
// __SANITIZE_ADDRESS__. LeakSanitizer is part of AddressSanitizer on both, and is also available
// standalone, in which case the interface header is present and the calls are no-ops — which is
// harmless and is why the call is not gated any more tightly than this.
#if defined(__SANITIZE_ADDRESS__)
#    define CY_LSAN_ACTIVE 1
#elif defined(__has_feature)
#    if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer)
#        define CY_LSAN_ACTIVE 1
#    endif
#endif

namespace cy {
namespace {

struct Registry {
    std::mutex mutex;
    ProcessLifetimeEntry entries[kMaxProcessLifetimeEntries];
    u32 count = 0;
    std::atomic<u64> bytes{0};
    std::atomic<u64> rejections{0};
};

/// Namespace scope, not a function-local static: a declaration can arrive from a constructor that
/// runs before main, and this way the storage is already there rather than being created by the
/// first caller. The type is trivially constructible apart from the mutex, which is
/// constant-initialised on every platform the engine targets.
Registry g_registry;

}  // namespace

void declare_process_lifetime(const void* pointer, u64 bytes, const char* tag) noexcept {
    if (pointer == nullptr) {
        return;
    }
    {
        const std::lock_guard<std::mutex> guard(g_registry.mutex);
        for (u32 index = 0; index < g_registry.count; ++index) {
            if (g_registry.entries[index].pointer == pointer) {
                return;  // already declared; a second declaration is not a second allocation
            }
        }
        if (g_registry.count >= kMaxProcessLifetimeEntries) {
            g_registry.rejections.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_registry.entries[g_registry.count++] =
            ProcessLifetimeEntry{pointer, bytes, (tag != nullptr) ? tag : ""};
        g_registry.bytes.fetch_add(bytes, std::memory_order_relaxed);
    }

#if defined(CY_HAS_LSAN_INTERFACE) && defined(CY_LSAN_ACTIVE)
    // Tell the second detector the same thing. The pointer must be the one the allocator returned;
    // LeakSanitizer resolves an interior pointer, but naming the block exactly is what makes this a
    // declaration rather than a guess.
    __lsan_ignore_object(pointer);
#endif
}

void withdraw_process_lifetime(const void* pointer) noexcept {
    const std::lock_guard<std::mutex> guard(g_registry.mutex);
    for (u32 index = 0; index < g_registry.count; ++index) {
        if (g_registry.entries[index].pointer != pointer) {
            continue;
        }
        g_registry.bytes.fetch_sub(g_registry.entries[index].bytes, std::memory_order_relaxed);
        g_registry.entries[index] = g_registry.entries[g_registry.count - 1];
        --g_registry.count;
        return;
    }
}

bool is_process_lifetime(const void* pointer) noexcept {
    const std::lock_guard<std::mutex> guard(g_registry.mutex);
    for (u32 index = 0; index < g_registry.count; ++index) {
        if (g_registry.entries[index].pointer == pointer) {
            return true;
        }
    }
    return false;
}

u64 process_lifetime_bytes() noexcept {
    return g_registry.bytes.load(std::memory_order_relaxed);
}

u32 process_lifetime_count() noexcept {
    const std::lock_guard<std::mutex> guard(g_registry.mutex);
    return g_registry.count;
}

u64 process_lifetime_rejections() noexcept {
    return g_registry.rejections.load(std::memory_order_relaxed);
}

u32 process_lifetime_entries(ProcessLifetimeEntry* out, u32 capacity) noexcept {
    const std::lock_guard<std::mutex> guard(g_registry.mutex);
    const u32 count = (g_registry.count < capacity) ? g_registry.count : capacity;
    for (u32 index = 0; index < count; ++index) {
        out[index] = g_registry.entries[index];
    }
    return count;
}

bool leak_sanitizer_present() noexcept {
#if defined(CY_LSAN_ACTIVE)
    return true;
#else
    return false;
#endif
}

}  // namespace cy
