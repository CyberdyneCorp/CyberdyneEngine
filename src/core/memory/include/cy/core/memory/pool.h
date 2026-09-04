#pragma once
// Fixed-size blocks for uniform objects. Task 2.1.
//
// `core-memory-and-containers` — "Allocator interface": `PoolAllocator<T>` provides fixed-size
// blocks for uniform objects. It is one of the four allocators that must cover the per-frame
// allocation pattern so the general heap stays the fallback rather than the hot path.
//
// Blocks come from a `ChunkAllocator`, so an object's address is stable for as long as it is live
// and the pool can grow without moving anything. A freed block goes on a free list threaded through
// its own storage: no header, no bitmap, and reuse in constant time.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/chunk_allocator.h>
#include <cy/core/memory/sanitizer.h>

#include <type_traits>
#include <utility>

namespace cy {

/// A pool of `T`-sized blocks.
///
/// `acquire`/`release` hand out raw storage; `create`/`destroy` construct and destruct in it. Both
/// pairs exist because the two callers are different: a container placing objects itself wants the
/// storage, and a subsystem wants the object.
template <class T>
class PoolAllocator final : public Allocator {
public:
    /// A block is at least a pointer, so the free list has somewhere to live even for a `T` smaller
    /// than one. Rounded up to `T`'s alignment so every block is correctly aligned for it.
    static constexpr usize kBlockBytes =
        align_up((sizeof(T) < sizeof(void*)) ? sizeof(void*) : sizeof(T), alignof(T));
    static constexpr usize kBlockAlignment =
        (alignof(T) < alignof(void*)) ? alignof(void*) : alignof(T);

    PoolAllocator(MemoryDomain domain, AllocationTag tag, u32 objects_per_chunk = 64) noexcept
        : Allocator(domain, tag),
          chunks_(domain, tag, chunk_bytes_for(objects_per_chunk), kBlockAlignment),
          objects_per_chunk_(objects_per_chunk) {}

    ~PoolAllocator() override = default;

    void set_upstream(Allocator& upstream) noexcept { chunks_.set_upstream(upstream); }

    /// THE HOT PATH. Uninitialised storage for one `T`, or null. Non-virtual and inline: a pop from
    /// a free list, and a chunk carve only when the list is empty.
    [[nodiscard]] T* acquire() noexcept {
        if (free_list_ == nullptr && !carve_chunk()) {
            return nullptr;
        }
        FreeNode* node = free_list_;
        // A free block is poisoned, including the link threaded through it. See sanitizer.h.
        unpoison_memory(node, kBlockBytes);
        free_list_ = node->next;
        ++live_;
        // Through void*: a direct reinterpret_cast between two pointer types of different
        // alignment is what -Wcast-align reports, and the block is aligned for both.
        // NOLINTNEXTLINE(bugprone-casting-through-void)
        return static_cast<T*>(static_cast<void*>(node));
    }

    /// Return storage to the pool. The object must already have been destroyed.
    void release(T* object) noexcept {
        if (object == nullptr) {
            return;
        }
        // NOLINTNEXTLINE(bugprone-casting-through-void) — see acquire().
        auto* node = static_cast<FreeNode*>(static_cast<void*>(object));
        node->next = free_list_;
        free_list_ = node;
        --live_;
        // Reading the block after this is a use-after-free, and now says so.
        poison_memory(node, kBlockBytes);
    }

    template <class... Args>
    [[nodiscard]] Expected<T*, Error> create(Args&&... args) noexcept {
        T* storage = acquire();
        if (storage == nullptr) {
            return fail(ErrorCode::OutOfMemory, "pool exhausted");
        }
        return construct_at<T>(storage, std::forward<Args>(args)...);
    }

    void destroy(T* object) noexcept {
        if (object == nullptr) {
            return;
        }
        object->~T();
        release(object);
    }

    [[nodiscard]] u32 live() const noexcept { return live_; }
    [[nodiscard]] u32 capacity() const noexcept {
        return chunks_.committed_chunks() * objects_per_chunk_;
    }
    [[nodiscard]] u64 committed_bytes() const noexcept { return chunks_.committed_bytes(); }

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override {
        // A pool of `T` allocates `T`s. Anything else is a caller error, and answering null is how
        // an allocator says so.
        if (size > kBlockBytes || alignment > kBlockAlignment) {
            return nullptr;
        }
        return acquire();
    }

    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override {
        (void)old_size;
        (void)alignment;
        return (new_size <= kBlockBytes) ? pointer : nullptr;
    }

    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override {
        (void)size;
        (void)alignment;
        release(static_cast<T*>(pointer));
    }

private:
    struct FreeNode {
        FreeNode* next;
    };

    static constexpr usize chunk_bytes_for(u32 objects_per_chunk) noexcept {
        return kBlockBytes * ((objects_per_chunk == 0) ? 1u : objects_per_chunk);
    }

    /// Take one chunk and thread every block in it onto the free list. Built back to front so the
    /// list runs forward through the chunk, which is what an allocation burst wants to touch.
    [[nodiscard]] bool carve_chunk() noexcept {
        auto* chunk = static_cast<u8*>(chunks_.acquire());
        if (chunk == nullptr) {
            return false;
        }
        for (u32 index = objects_per_chunk_; index > 0; --index) {
            void* slot = chunk + (static_cast<usize>(index - 1) * kBlockBytes);
            auto* node = static_cast<FreeNode*>(slot);
            node->next = free_list_;
            free_list_ = node;
            poison_memory(node, kBlockBytes);
        }
        return true;
    }

    ChunkAllocator chunks_;
    FreeNode* free_list_ = nullptr;
    u32 objects_per_chunk_;
    u32 live_ = 0;
};

}  // namespace cy
