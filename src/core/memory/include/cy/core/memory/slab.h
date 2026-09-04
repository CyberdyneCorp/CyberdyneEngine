#pragma once
// Per-worker slabs for task records, coroutine frames and events. Task 2.1.
//
// `core-memory-and-containers` — "Allocator interface": `SlabAllocator` covers per-worker slabs for
// task records, coroutine frames and events. The distinction from `ArenaAllocator` is growth: an
// arena has one block and a fixed capacity, and a slab allocator chains blocks, so a worker whose
// task graph is deeper than expected slows down rather than failing.
//
// Freeing is by `reset()`, whole slabs at a time. `deallocate` is a no-op, exactly as in an arena:
// the lifetime this allocator serves is "until the job is done", and the job saying so once is
// cheaper and less error-prone than every allocation being released individually.
//
// SINGLE THREADED BY CONSTRUCTION. One slab allocator per worker; nothing here is synchronised, and
// nothing needs to be, because the worker that owns it is the only thread that touches it.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/arena.h>

namespace cy {

class SlabAllocator final : public Allocator {
public:
    /// `slab_bytes` is the size of each block taken from upstream. An allocation larger than this
    /// gets a slab of its own, sized to it, so a large coroutine frame is served rather than
    /// refused.
    SlabAllocator(MemoryDomain domain, AllocationTag tag,
                  usize slab_bytes = usize{64} * 1024) noexcept;
    ~SlabAllocator() override;

    void set_upstream(Allocator& upstream) noexcept;

    /// THE HOT PATH. A bump in the current slab; a new slab only when this one is full.
    [[nodiscard]] void* take(usize size, usize alignment = kDefaultAlignment) noexcept {
        if (current_ != nullptr) {
            void* fast = current_->region.bump(size, alignment);
            if (fast != nullptr) {
                return fast;
            }
        }
        return take_slow(size, alignment);
    }

    /// Rewind every slab to empty, keeping them for reuse. O(number of slabs), which is the point:
    /// a worker's whole scratch state is discarded between jobs without touching the heap.
    void reset() noexcept;

    /// Return every slab but the first to upstream. The `Critical` pressure response for a worker
    /// that ran one deep job and will not run another.
    usize trim() noexcept;

    [[nodiscard]] usize slab_count() const noexcept { return slab_count_; }
    [[nodiscard]] u64 committed_bytes() const noexcept { return committed_bytes_; }
    [[nodiscard]] u64 used_bytes() const noexcept;

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override;
    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override;
    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override;

private:
    /// One slab: its own bump region, and the link to the next. The header lives at the front of
    /// the block it describes, so a slab costs one upstream allocation rather than two.
    struct Slab {
        Slab* next = nullptr;
        usize bytes = 0;  // the whole block, header included
        detail::BumpRegion region;
    };

    [[nodiscard]] void* take_slow(usize size, usize alignment) noexcept;
    [[nodiscard]] Slab* allocate_slab(usize payload_bytes) noexcept;

    usize slab_bytes_;
    Allocator* upstream_ = nullptr;
    Slab* head_ = nullptr;     // every slab, newest first
    Slab* current_ = nullptr;  // the slab `take` bumps in; always one of the chain from head_
    usize slab_count_ = 0;
    u64 committed_bytes_ = 0;
};

}  // namespace cy
