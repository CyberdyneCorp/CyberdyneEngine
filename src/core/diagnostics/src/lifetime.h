#pragma once
// Allocations this module holds for the life of the process, declared where the leak detector can
// see the declaration. Task 2.12, carried from M0.
//
// `testing-and-quality` — "Memory and concurrency correctness": "A subsystem that intentionally
// holds an allocation for the life of the process — a pooled ring, a registry, an interned table —
// SHALL declare it where the leak detector can see the declaration, so that a report is either a
// real defect or a declared exception, and never a standing failure the suite is expected to
// tolerate."
//
// WHAT IS DECLARED, AND WHY IT LOOKS LIKE A LEAK. A thread that emits its first record claims a
// `ThreadSlot`, and the slot owns a ring buffer. When the thread ends the slot goes back on the
// free list rather than being freed, so the next thread reuses the buffer instead of allocating
// another one — a process that churns threads does not churn 64 KiB buffers. The vector holding the
// slots is a member of a function-local static whose destructor runs before LeakSanitizer's check,
// so at the moment the tool looks, nothing points at either allocation. That is pooling, not a
// defect, and this is where the engine says so.
//
// WHY NOT `cy::declare_process_lifetime()`. That is the engine's general mechanism, and it is the
// right one wherever it can be called: it names the pointer to LeakSanitizer *and* to the engine's
// own leak report. It lives in cy::core-memory, which sits above this module in the link order —
// diagnostics is what memory reports through — so calling it here would close a dependency cycle.
// The tool half of that mechanism is three lines, so this module carries its own copy of the tool
// half rather than acquiring a dependency it exists to serve.
//
// WHY NOT A SUPPRESSION FILE. A suppression matches a frame and hides every allocation that passes
// through it, including the defect that allocates one more ring every frame. This names one
// pointer. It also needs no environment variable, so a developer running a sanitized binary by hand
// gets the same answer continuous integration does.
//
// EVERY FUNCTION HERE IS EMPTY IN AN UNINSTRUMENTED BINARY.

#include <cy/core/diagnostics/prelude.h>

#if defined(__has_feature)
#    if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer)
#        define CY_DIAG_LSAN_CANDIDATE 1
#    endif
#endif
#if !defined(CY_DIAG_LSAN_CANDIDATE) && \
    (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_LEAK__))
#    define CY_DIAG_LSAN_CANDIDATE 1
#endif

// The interface header ships with the compiler's sanitizer runtime rather than with the C library,
// so its presence is checked rather than assumed: a toolchain that instruments without shipping it
// still compiles, and simply declares nothing.
#if defined(CY_DIAG_LSAN_CANDIDATE) && defined(__has_include)
#    if __has_include(<sanitizer/lsan_interface.h>)
#        include <sanitizer/lsan_interface.h>
#        define CY_DIAG_LSAN 1
#    endif
#endif

namespace cy::diag {

/// Whether this binary can tell the leak detector about a pooled allocation. Reported by the
/// trace's own diagnostics so that "declared" is a fact a test can check rather than an assumption.
inline constexpr bool kLeakDetectorDeclarationAvailable =
#if defined(CY_DIAG_LSAN)
    true;
#else
    false;
#endif

/// Declare that `pointer` is held deliberately until the process exits. Idempotent, safe from any
/// thread, and safe on a null pointer — the caller checks its allocation for its own reasons, not
/// for this one's.
inline void declare_process_lifetime(const void* pointer) noexcept {
#if defined(CY_DIAG_LSAN)
    if (pointer != nullptr) {
        __lsan_ignore_object(pointer);
    }
#else
    (void)pointer;
#endif
}

}  // namespace cy::diag
