#pragma once
// Fixed-size chunks with stable addresses. Task 2.1; the storage half of task 2.6.
//
// `core-memory-and-containers` — "Allocator interface": `ChunkAllocator` provides fixed-size chunks
// with stable addresses and backs ECS storage and handle pools. Stability is the whole point: a
// chunk that has been handed out is never moved and never freed while the allocator lives, so a
// pointer into one stays valid across any amount of growth elsewhere.
//
// Chunks come from the upstream allocator one block at a time rather than in a growing array, so a
// chunk's address does not depend on how many chunks exist. Released chunks go on a free list
// threaded through their own first bytes and are reused before another is taken from upstream.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>

namespace cy {

/// The default chunk size. 16 KiB is the figure `core-memory-and-containers` names for component
/// storage; a handle pool with a different object size passes its own.
inline constexpr usize kDefaultChunkBytes = 16u * 1024u;

/// The most chunks one allocator addresses. The pointer array is fixed at this size and
/// pre-reserved so that growth never reallocates it under a concurrent reader — the property
/// "Concurrent resolve during growth" asks for, expressed as the absence of a reallocation rather
/// than as a lock.
inline constexpr u32 kMaxChunksPerAllocator = 4096;

class ChunkAllocator final : public Allocator {
public:
    ChunkAllocator(MemoryDomain domain, AllocationTag tag, usize chunk_bytes = kDefaultChunkBytes,
                   usize chunk_alignment = 64) noexcept;
    ~ChunkAllocator() override;

    /// Point this allocator at the upstream it takes chunks from. Defaults to the system allocator
    /// for its own domain; call before the first acquire, or not at all.
    void set_upstream(Allocator& upstream) noexcept;

    /// THE HOT PATH. One chunk, or null when upstream refuses. Non-virtual: a caller with the
    /// concrete type pays a load and a branch on the free list.
    [[nodiscard]] void* acquire() noexcept;

    /// Return a chunk. It stays committed and goes on the free list; the address is reused, which
    /// is what keeps a churning archetype from touching the platform heap every frame.
    void release(void* chunk) noexcept;

    /// Take back every chunk the free list holds, returning them to upstream. The only way this
    /// allocator gives memory back before it is destroyed, and therefore what a `Critical` pressure
    /// response calls.
    usize trim() noexcept;

    [[nodiscard]] usize chunk_bytes() const noexcept { return chunk_bytes_; }
    [[nodiscard]] u32 committed_chunks() const noexcept { return committed_; }
    [[nodiscard]] u32 live_chunks() const noexcept { return committed_ - free_count_; }
    [[nodiscard]] u32 free_chunks() const noexcept { return free_count_; }
    /// Committed bytes, which is what this allocator costs its domain.
    [[nodiscard]] u64 committed_bytes() const noexcept {
        return static_cast<u64>(committed_) * chunk_bytes_;
    }

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override;
    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override;
    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override;

private:
    /// The free list is threaded through the chunks themselves: a free chunk's first bytes hold the
    /// address of the next free one. A chunk is at least a page, so there is always room, and the
    /// list costs no memory of its own.
    struct FreeNode {
        FreeNode* next;
    };

    [[nodiscard]] Allocator& upstream() noexcept;

    /// Take the chunk-pointer array from upstream, once, at its full size. False when upstream
    /// refuses it, which makes the first acquire fail rather than half-succeed.
    [[nodiscard]] bool ensure_directory() noexcept;

    usize chunk_bytes_;
    usize chunk_alignment_;
    Allocator* upstream_ = nullptr;
    FreeNode* free_list_ = nullptr;
    u32 free_count_ = 0;
    u32 committed_ = 0;
    /// Every chunk this allocator has taken from upstream, so the destructor can give them back.
    /// Allocated once at its full size on the first acquire and never reallocated — an inline array
    /// would put 32 KiB inside every pool, and a growing one would move under a reader.
    void** chunks_ = nullptr;
};

}  // namespace cy
