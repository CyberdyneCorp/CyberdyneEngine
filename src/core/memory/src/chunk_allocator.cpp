// Fixed-size chunks with stable addresses. Task 2.1.

#include <cy/core/memory/chunk_allocator.h>

#include <cy/core/memory/system_allocator.h>

namespace cy {

ChunkAllocator::ChunkAllocator(MemoryDomain domain, AllocationTag tag, usize chunk_bytes,
                               usize chunk_alignment) noexcept
    : Allocator(domain, tag), chunk_bytes_(chunk_bytes), chunk_alignment_(chunk_alignment) {
    CY_ASSERT_MSG(chunk_bytes >= sizeof(FreeNode), "a chunk must hold a free-list link");
    CY_ASSERT_MSG(is_power_of_two(chunk_alignment), "chunk alignment must be a power of two");
}

ChunkAllocator::~ChunkAllocator() {
    for (u32 index = 0; index < committed_; ++index) {
        upstream().deallocate(chunks_[index], chunk_bytes_, chunk_alignment_);
    }
    if (chunks_ != nullptr) {
        upstream().deallocate(chunks_, kMaxChunksPerAllocator * sizeof(void*), alignof(void*));
        chunks_ = nullptr;
    }
    committed_ = 0;
    free_count_ = 0;
    free_list_ = nullptr;
}

bool ChunkAllocator::ensure_directory() noexcept {
    if (chunks_ != nullptr) {
        return true;
    }
    void* block = upstream().allocate(kMaxChunksPerAllocator * sizeof(void*), alignof(void*));
    if (block == nullptr) {
        return false;
    }
    chunks_ = static_cast<void**>(block);
    for (u32 index = 0; index < kMaxChunksPerAllocator; ++index) {
        chunks_[index] = nullptr;
    }
    return true;
}

Allocator& ChunkAllocator::upstream() noexcept {
    return (upstream_ != nullptr) ? *upstream_ : system_allocator(domain_);
}

void ChunkAllocator::set_upstream(Allocator& source) noexcept {
    CY_ASSERT_MSG(committed_ == 0, "the upstream allocator is chosen before the first chunk");
    upstream_ = &source;
}

void* ChunkAllocator::acquire() noexcept {
    if (free_list_ != nullptr) {
        FreeNode* node = free_list_;
        free_list_ = node->next;
        --free_count_;
        return node;
    }
    if (committed_ >= kMaxChunksPerAllocator || !ensure_directory()) {
        return nullptr;
    }
    void* chunk = upstream().allocate(chunk_bytes_, chunk_alignment_);
    if (chunk == nullptr) {
        return nullptr;
    }
    chunks_[committed_++] = chunk;
    return chunk;
}

void ChunkAllocator::release(void* chunk) noexcept {
    if (chunk == nullptr) {
        return;
    }
    auto* node = static_cast<FreeNode*>(chunk);
    node->next = free_list_;
    free_list_ = node;
    ++free_count_;
}

usize ChunkAllocator::trim() noexcept {
    // Chunks are given back to upstream, and the record of them in chunks_ is compacted so the
    // destructor does not free them twice. A free chunk's address is in the free list and in
    // chunks_; the sweep below is what reconciles the two, and it is O(free * committed) — which is
    // acceptable because trimming happens under memory pressure, not in a frame.
    usize released = 0;
    while (free_list_ != nullptr) {
        FreeNode* node = free_list_;
        free_list_ = node->next;
        --free_count_;

        for (u32 index = 0; index < committed_; ++index) {
            if (chunks_[index] != static_cast<void*>(node)) {
                continue;
            }
            chunks_[index] = chunks_[committed_ - 1];
            chunks_[--committed_] = nullptr;
            break;
        }
        upstream().deallocate(node, chunk_bytes_, chunk_alignment_);
        ++released;
    }
    return released;
}

void* ChunkAllocator::do_allocate(usize size, usize alignment) noexcept {
    // A chunk allocator hands out chunks. A request for more than one holds is a caller error the
    // interface can report rather than satisfy badly.
    if (size > chunk_bytes_ || alignment > chunk_alignment_) {
        return nullptr;
    }
    return acquire();
}

void* ChunkAllocator::do_reallocate(void* pointer, usize old_size, usize new_size,
                                    usize alignment) noexcept {
    (void)old_size;
    (void)alignment;
    // Every block is one chunk, so a resize that still fits is the same block and one that does not
    // cannot be satisfied. Copying into a second chunk would silently break the address stability
    // this allocator exists to provide.
    return (new_size <= chunk_bytes_) ? pointer : nullptr;
}

void ChunkAllocator::do_deallocate(void* pointer, usize size, usize alignment) noexcept {
    (void)size;
    (void)alignment;
    release(pointer);
}

}  // namespace cy
