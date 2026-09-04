// Per-worker slabs. Task 2.1.
//
// A slab is one upstream block: its header at the front, its bump region over the rest. `take`
// bumps in the current slab; when that fails, `take_slow` looks for an earlier slab with room —
// which is the case after a `reset()` — and only then asks upstream for another.

#include <cy/core/memory/slab.h>

#include <cy/core/memory/system_allocator.h>

namespace cy {

SlabAllocator::SlabAllocator(MemoryDomain domain, AllocationTag tag, usize slab_bytes) noexcept
    : Allocator(domain, tag), slab_bytes_(slab_bytes) {}

SlabAllocator::~SlabAllocator() {
    Slab* slab = head_;
    while (slab != nullptr) {
        Slab* next = slab->next;
        const usize bytes = slab->bytes;
        Allocator& source = (upstream_ != nullptr) ? *upstream_ : system_allocator(domain_);
        slab->region.unpoison_all();  // upstream must not receive a block this one left poisoned
        slab->~Slab();
        source.deallocate(slab, bytes, kDefaultAlignment);
        slab = next;
    }
    head_ = nullptr;
    current_ = nullptr;
    slab_count_ = 0;
    committed_bytes_ = 0;
}

void SlabAllocator::set_upstream(Allocator& upstream) noexcept {
    CY_ASSERT_MSG(head_ == nullptr, "the upstream allocator is chosen before the first slab");
    upstream_ = &upstream;
}

SlabAllocator::Slab* SlabAllocator::allocate_slab(usize payload_bytes) noexcept {
    Allocator& source = (upstream_ != nullptr) ? *upstream_ : system_allocator(domain_);
    const usize total = align_up(sizeof(Slab), kDefaultAlignment) + payload_bytes;
    void* block = source.allocate(total, kDefaultAlignment);
    if (block == nullptr) {
        return nullptr;
    }
    auto* slab = construct_at<Slab>(block);
    slab->next = head_;
    slab->bytes = total;
    slab->region.adopt(static_cast<u8*>(block) + align_up(sizeof(Slab), kDefaultAlignment),
                       payload_bytes);
    head_ = slab;
    ++slab_count_;
    committed_bytes_ += total;
    return slab;
}

void* SlabAllocator::take_slow(usize size, usize alignment) noexcept {
    // After a reset there are empty slabs behind the current one; use them before asking upstream.
    for (Slab* slab = head_; slab != nullptr; slab = slab->next) {
        if (slab == current_) {
            continue;
        }
        void* fitted = slab->region.bump(size, alignment);
        if (fitted != nullptr) {
            current_ = slab;
            return fitted;
        }
    }

    // An allocation larger than the standard slab gets one sized to it, rather than being refused
    // by an allocator whose whole job is to serve a worker's temporaries.
    const usize payload = (size + alignment > slab_bytes_) ? (size + alignment) : slab_bytes_;
    Slab* slab = allocate_slab(payload);
    if (slab == nullptr) {
        return nullptr;
    }
    current_ = slab;
    return slab->region.bump(size, alignment);
}

void SlabAllocator::reset() noexcept {
    for (Slab* slab = head_; slab != nullptr; slab = slab->next) {
        slab->region.reset();
    }
    current_ = head_;
}

usize SlabAllocator::trim() noexcept {
    if (head_ == nullptr) {
        return 0;
    }
    Allocator& source = (upstream_ != nullptr) ? *upstream_ : system_allocator(domain_);
    usize released = 0;
    Slab* slab = head_->next;
    while (slab != nullptr) {
        Slab* next = slab->next;
        const usize bytes = slab->bytes;
        committed_bytes_ -= bytes;
        --slab_count_;
        slab->~Slab();
        source.deallocate(slab, bytes, kDefaultAlignment);
        ++released;
        slab = next;
    }
    head_->next = nullptr;
    head_->region.reset();
    current_ = head_;
    return released;
}

u64 SlabAllocator::used_bytes() const noexcept {
    u64 total = 0;
    for (const Slab* slab = head_; slab != nullptr; slab = slab->next) {
        total += slab->region.used();
    }
    return total;
}

void* SlabAllocator::do_allocate(usize size, usize alignment) noexcept {
    return take(size, alignment);
}

void* SlabAllocator::do_reallocate(void* pointer, usize old_size, usize new_size,
                                   usize alignment) noexcept {
    return reallocate_by_copy(pointer, old_size, new_size, alignment);
}

void SlabAllocator::do_deallocate(void* pointer, usize size, usize alignment) noexcept {
    // A slab is freed whole, by reset(). Individual release is deliberately nothing.
    (void)pointer;
    (void)size;
    (void)alignment;
}

}  // namespace cy
