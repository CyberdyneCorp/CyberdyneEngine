#pragma once
// Allocations that are held for the life of the process on purpose. Tasks 2.11 and 2.12.
//
// `core-memory-and-containers` — "Memory diagnostics": "Allocations that are intentionally held for
// the process lifetime SHALL be taggable as such, so leak reports distinguish them from defects."
//
// TWO DETECTORS, ONE DECLARATION. The engine's own leak report (`TrackingAllocator`) and
// LeakSanitizer are both told by this one call: the entry goes into the registry the report reads,
// and, when the binary is built with LeakSanitizer, `__lsan_ignore_object` is called for the same
// pointer. A subsystem therefore declares an intentional lifetime once and is believed by both,
// rather than declaring it to the engine and being contradicted by the tool.
//
// PRECISION IS THE WHOLE POINT. This is not a suppression: it names one pointer, of a known size,
// with a tag that appears in the report as an intentional allocation rather than being hidden. A
// blanket suppression would also hide the defect that allocated one more of them every frame.
//
// THE M0 CARRY-OVER. The trace's per-thread ring buffers are pooled for the life of the process by
// design — a thread that exits returns its slot for the next thread rather than freeing it, so a
// process that churns threads does not churn buffers. LeakSanitizer sees the pool as unreachable at
// exit, because the container holding it is itself destroyed first. That module sits BELOW this one
// in the link order — memory reports onto the trace — so it cannot call this function without
// closing a cycle. It carries the tool half itself instead, in
// `src/core/diagnostics/src/lifetime.h`, and declares both the slot and its ring at the allocation
// site. Use this function everywhere the dependency is legal, because it reaches the engine's own
// leak report as well as the tool's.

#include <cy/core/base/types.h>

namespace cy {

/// One declared allocation.
struct ProcessLifetimeEntry {
    const void* pointer = nullptr;
    u64 bytes = 0;
    const char* tag = "";  // a literal, or storage that outlives the process
};

/// The most declarations the registry holds. Process-lifetime allocations are pools, tables and
/// rings — a handful per subsystem — so a fixed table is the right shape and refusing the
/// hundred-and-first is a report rather than a resize.
inline constexpr u32 kMaxProcessLifetimeEntries = 256;

/// Declare that `pointer` is held deliberately until the process exits.
///
/// Idempotent for a pointer already declared. Safe to call before main and from any thread.
void declare_process_lifetime(const void* pointer, u64 bytes, const char* tag) noexcept;

/// Withdraw a declaration — for a pool that is, after all, being freed.
void withdraw_process_lifetime(const void* pointer) noexcept;

[[nodiscard]] bool is_process_lifetime(const void* pointer) noexcept;

[[nodiscard]] u64 process_lifetime_bytes() noexcept;
[[nodiscard]] u32 process_lifetime_count() noexcept;
/// How many declarations were refused because the table was full.
[[nodiscard]] u64 process_lifetime_rejections() noexcept;

/// Copy the declarations into `out`, newest last. Returns how many were written.
u32 process_lifetime_entries(ProcessLifetimeEntry* out, u32 capacity) noexcept;

/// Whether this binary was built with LeakSanitizer, so a report can say whether the declaration
/// reached a second detector or only the engine's own.
[[nodiscard]] bool leak_sanitizer_present() noexcept;

}  // namespace cy
