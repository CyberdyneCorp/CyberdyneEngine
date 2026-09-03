#pragma once
// The counters `values_diagnostics()` reports. Task 1.3.6.
//
// Private to the module. They live in one place rather than one per source so that the report is a
// snapshot of a fixed set rather than a union of whatever each file happened to keep, and so that
// adding a counter is a change to this header and to the report, together.
//
// Every counter is relaxed-atomic. Relaxed is the right ordering: nothing here synchronises
// anything, a counter is read by the reporter and by a test, and paying for acquire/release on the
// increment would put a real cost on paths — handle validation above all — that must not have one.

#include <cy/core/base/types.h>

#include <atomic>

namespace cy::values::detail {

struct Counters {
    // Interning (name.cpp).
    std::atomic<u64> name_lookups{0};
    std::atomic<u64> name_insertions{0};
    std::atomic<u64> name_rejections{0};

    // Var's heap blocks (var.cpp).
    std::atomic<u64> var_blocks_allocated{0};
    std::atomic<u64> var_blocks_freed{0};
    std::atomic<u64> var_blocks_detached{0};  // copy-on-write clones

    // Handles (handle.cpp).
    std::atomic<u64> handle_slots_allocated{0};
    std::atomic<u64> handle_slots_freed{0};
    std::atomic<u64> handle_chunks_committed{0};
    std::atomic<u64> stale_handle_rejections{0};

    // Signals (signal.cpp).
    std::atomic<u64> signal_emissions{0};
    std::atomic<u64> signal_invocations{0};
    std::atomic<u64> signal_deferred{0};
    std::atomic<u64> signal_connections_pruned{0};

    // Callables (callable.cpp).
    std::atomic<u64> call_invocations{0};
    std::atomic<u64> call_failures{0};
};

/// The one instance. A function-local static rather than a namespace-scope object so that a counter
/// bumped from another translation unit's static initialiser cannot reach it before construction.
Counters& counters() noexcept;

inline void bump(std::atomic<u64>& counter, u64 amount = 1) noexcept {
    counter.fetch_add(amount, std::memory_order_relaxed);
}

}  // namespace cy::values::detail
