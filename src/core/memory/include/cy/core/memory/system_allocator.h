#pragma once
// The backing allocator over the platform heap. Tasks 2.1 and 2.10.
//
// `core-memory-and-containers` — "General heap is an integration decided by measurement": the heap
// behind `SystemAllocator` is chosen by benchmark and stays replaceable behind this interface. The
// measurement, and its numbers, are in `src/core/memory/README.md`; `bench/heap_pattern.cpp` is the
// benchmark that produced them and is what makes a later regression visible.
//
// This is the FALLBACK, not the hot path. Per-frame and per-task allocation comes from arenas,
// scratch, pools and chunks; a general-heap allocation in a frame is rare and, because every one of
// them is recorded against a domain here, attributable.

#include <cy/core/memory/allocator.h>
#include <cy/core/memory/domain.h>

#include <atomic>

namespace cy {

/// Aligned allocation over the platform heap, with every block recorded against `domain()`.
///
/// `final` on purpose: a caller holding a `SystemAllocator&` gets a direct call, and nothing is
/// gained by letting a subclass reimplement malloc. Wrap it instead — that is what
/// `TrackingAllocator` is.
class SystemAllocator final : public Allocator {
public:
    SystemAllocator(MemoryDomain domain, AllocationTag tag) noexcept : Allocator(domain, tag) {}

    /// Live bytes this instance is holding. The per-domain figure is the one a report uses; this
    /// one answers "is this particular allocator the source", which is a different question.
    [[nodiscard]] u64 live_bytes() const noexcept;
    [[nodiscard]] u64 live_allocations() const noexcept;

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override;
    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override;
    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override;

private:
    // Relaxed atomics, because the per-domain instances returned by `system_allocator()` are
    // process-wide and several threads allocate through one of them at once. Relaxed is enough: the
    // counters order nothing, they are read as a snapshot, and the platform heap underneath does
    // its own synchronisation.
    std::atomic<u64> live_bytes_{0};
    std::atomic<u64> live_allocations_{0};
};

/// The process-wide fallback: domain `Engine`, tag "engine". Anything that has not been given an
/// allocator ends up here, which is exactly why the domain is the root one.
[[nodiscard]] SystemAllocator& default_allocator() noexcept;

/// A system allocator attributed to `domain`. One instance per domain, created on first use and
/// alive for the process, so a subsystem can push an allocator scope carrying its domain without
/// first having to own an arena.
[[nodiscard]] SystemAllocator& system_allocator(MemoryDomain domain) noexcept;

}  // namespace cy
