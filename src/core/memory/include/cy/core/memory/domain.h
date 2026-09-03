#pragma once
// Memory domains, and the per-domain accounting every allocation is attributed to. Task 2.2.
//
// `core-memory-and-containers` — "Memory domains": every allocation is attributed to a domain,
// declared rather than inferred; domains are hierarchical so a figure can be reported in aggregate
// or broken down; and attribution is available in ALL builds, because a shipping build that cannot
// say where its memory went cannot be diagnosed on the platform where it failed.
//
// The fourteen domains below are the specification's list, spelled the way it spells them. Two of
// them are children rather than siblings — `Gpu` under `Renderer`, `Streaming` under `Assets` —
// which is what makes the hierarchy load-bearing rather than decorative: "how much is the renderer
// using" and "how much of that is GPU-side" are different questions with one answer each.
//
// THE COST OF ATTRIBUTION. A record is one relaxed fetch_add on a counter the domain owns, plus a
// compare-exchange loop only when a new peak is set. It is compiled into every configuration. The
// hot paths do not pay it per allocation: an arena charges its domain once when it acquires its
// block and bump-allocates out of it for free (see arena.h), which is the same reason the budget
// tree can be read every frame without measuring it.

#include <cy/core/base/types.h>

namespace cy {

/// Where an allocation belongs. The value is an index into the accounting table, so the enumerators
/// are contiguous and `Count` is the size of that table.
enum class MemoryDomain : u8 {
    Engine = 0,  // the root: everything that has no more specific home
    Ecs,
    Frame,
    Renderer,
    Gpu,  // child of Renderer
    Physics,
    Animation,
    Audio,
    Assets,
    Streaming,  // child of Assets
    World,
    Network,
    Scripting,
    Editor,
    Count,
};

inline constexpr u32 kMemoryDomainCount = static_cast<u32>(MemoryDomain::Count);

/// The domain's own spelling, for a report. Never null, including for an out-of-range value.
[[nodiscard]] const char* domain_name(MemoryDomain domain) noexcept;

/// The domain that contains `domain`. `Engine` is its own parent — it is the root, and a walk
/// upward therefore terminates on it rather than on a sentinel the caller has to know about.
[[nodiscard]] MemoryDomain domain_parent(MemoryDomain domain) noexcept;

/// Whether `domain` is `ancestor` or is contained by it, directly or transitively.
[[nodiscard]] bool domain_is_within(MemoryDomain domain, MemoryDomain ancestor) noexcept;

/// What one domain has accounted for. `reserved_bytes` is address space that has been reserved and
/// not committed; it is reported here and deliberately excluded from `live_bytes`, because reserved
/// address space is not memory in use and must not count against a budget.
struct DomainStats {
    u64 live_bytes = 0;
    u64 peak_bytes = 0;
    u64 live_allocations = 0;
    u64 total_allocations = 0;
    u64 total_bytes = 0;
    u64 reserved_bytes = 0;
};

/// Record an allocation against a domain. Called by the allocator that owns real memory — the
/// system allocator, and any allocator at the moment it acquires backing — never by a bump.
void domain_record_allocation(MemoryDomain domain, u64 bytes) noexcept;
void domain_record_free(MemoryDomain domain, u64 bytes) noexcept;

/// Record a change in reserved-but-uncommitted address space. `delta` is signed: a release passes a
/// negative value. Reserved space never touches live_bytes.
void domain_record_reservation(MemoryDomain domain, i64 delta) noexcept;

/// This domain's own figures, excluding its children.
[[nodiscard]] DomainStats domain_stats(MemoryDomain domain) noexcept;

/// This domain's figures with every descendant folded in. `domain_stats_recursive(Engine)` is the
/// whole process, which is what the budget tree's root is compared against.
[[nodiscard]] DomainStats domain_stats_recursive(MemoryDomain domain) noexcept;

/// Reset every peak to the current live figure. A frame-scoped or level-scoped measurement wants a
/// peak since a moment, not since process start.
void reset_domain_peaks() noexcept;

/// Zero every counter. Tests only: a process that does this while allocations are outstanding will
/// report negative-looking figures when they are freed.
void reset_domain_accounting() noexcept;

}  // namespace cy
