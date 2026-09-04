#pragma once
// Routing the engine's own allocators through AddressSanitizer's interface. Task 2.11.
//
// `core-memory-and-containers` — "Memory diagnostics", scenario "Sanitiser build": "WHEN the engine
// is built with `CY_SANITIZE=address` THEN custom allocators SHALL route through the sanitiser's
// interface so overflows and use-after-free are reported accurately."
//
// WHY A CUSTOM ALLOCATOR NEEDS THIS. AddressSanitizer knows about the blocks the platform heap
// hands out. An arena takes ONE such block and carves ten thousand allocations from it, so as far
// as the tool is concerned the whole region is one valid object: a write that runs off the end of
// one bump allocation lands in the next and is not a finding, and a read from a freed pool block
// reads the block's own bytes and is not a finding either. Poisoning the bytes that are NOT
// currently handed out restores the reporting the tool would have given over malloc: the alignment
// padding between two bump allocations, a pool block on the free list, a chunk on the free list and
// everything past an arena's reset are use-after-poison rather than silently valid memory.
//
// EVERY FUNCTION HERE IS EMPTY IN A BINARY THAT IS NOT INSTRUMENTED, so the calls on the hot paths
// cost nothing in the configurations that ship. There is no runtime flag: the instrumentation is a
// property of the build, and a check for it would be a branch that is always taken the same way.
//
// WHAT IS DELIBERATELY NOT POISONED. `VirtualArena` reserves address space and commits pages as it
// grows (virtual_memory.h). Its bytes never came from the heap, and shadow state for an address
// range that is later unmapped is not reliably reset for the next mapping — poisoning there risks a
// false positive on memory the process legitimately re-maps, which is worse than the finding it
// would add. The arena, stack, slab, pool and chunk allocators all carve heap blocks, where the
// platform allocator's own unpoison on allocation is what makes this safe.

#include <cy/core/base/types.h>

#if defined(__has_feature)
#    if __has_feature(address_sanitizer)
#        define CY_MEMORY_ASAN 1
#    endif
#endif
#if !defined(CY_MEMORY_ASAN) && defined(__SANITIZE_ADDRESS__)
#    define CY_MEMORY_ASAN 1
#endif

#if defined(CY_MEMORY_ASAN)
#    include <sanitizer/asan_interface.h>
#endif

namespace cy {

/// Whether this binary is instrumented, so a report can say whether the poisoning below is live.
inline constexpr bool kAddressSanitizerPresent =
#if defined(CY_MEMORY_ASAN)
    true;
#else
    false;
#endif

/// Mark bytes an allocator owns but has not handed out. Reading or writing them is then a
/// use-after-poison finding naming the access, rather than a silent success.
inline void poison_memory(const void* pointer, usize bytes) noexcept {
#if defined(CY_MEMORY_ASAN)
    if (pointer != nullptr && bytes != 0) {
        __asan_poison_memory_region(pointer, bytes);
    }
#else
    (void)pointer;
    (void)bytes;
#endif
}

/// Hand bytes back to the program. Called before an allocation is returned to a caller, and before
/// a block goes back to the allocator underneath — memory must never leave here poisoned.
inline void unpoison_memory(const void* pointer, usize bytes) noexcept {
#if defined(CY_MEMORY_ASAN)
    if (pointer != nullptr && bytes != 0) {
        __asan_unpoison_memory_region(pointer, bytes);
    }
#else
    (void)pointer;
    (void)bytes;
#endif
}

/// Whether one address is currently poisoned. Always false in an uninstrumented build, which is why
/// a test written against it asserts `kAddressSanitizerPresent` first.
[[nodiscard]] inline bool memory_is_poisoned(const void* pointer) noexcept {
#if defined(CY_MEMORY_ASAN)
    return __asan_address_is_poisoned(pointer) != 0;
#else
    (void)pointer;
    return false;
#endif
}

}  // namespace cy
