// The arena and the stack. Tasks 2.1 and 2.7.

#include <cy/core/memory/arena.h>

#include <cy/core/memory/system_allocator.h>

#include <cstring>

namespace cy {

namespace detail {

void BumpRegion::poison(usize offset, usize bytes) noexcept {
#if defined(CY_DEVELOPMENT)
    if (data_ != nullptr && bytes != 0) {
        std::memset(data_ + offset, kArenaPoisonByte, bytes);
    }
#else
    (void)offset;
    (void)bytes;
#endif
}

}  // namespace detail

// --- ArenaAllocator
// -------------------------------------------------------------------------------

ArenaAllocator::~ArenaAllocator() {
    release();
}

void ArenaAllocator::release() noexcept {
    if (upstream_ != nullptr && region_.data() != nullptr) {
        upstream_->deallocate(region_.data(), region_.capacity(), kDefaultAlignment);
    }
    upstream_ = nullptr;
    region_.adopt(nullptr, 0);
}

Status ArenaAllocator::reserve(usize bytes, Allocator& upstream) noexcept {
    release();
    if (bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "an arena of zero bytes allocates nothing");
    }
    void* block = upstream.allocate(bytes, kDefaultAlignment);
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "arena reservation failed");
    }
    upstream_ = &upstream;
    region_.adopt(static_cast<u8*>(block), bytes);
    return ok();
}

Status ArenaAllocator::reserve(usize bytes) noexcept {
    return reserve(bytes, system_allocator(domain_));
}

void ArenaAllocator::adopt(void* buffer, usize bytes) noexcept {
    release();
    region_.adopt(static_cast<u8*>(buffer), bytes);
}

void* ArenaAllocator::do_allocate(usize size, usize alignment) noexcept {
    return region_.bump(size, alignment);
}

void* ArenaAllocator::do_reallocate(void* pointer, usize old_size, usize new_size,
                                    usize alignment) noexcept {
    // Growing the most recent allocation in place is the case that matters: it is what a container
    // filling an arena does, and without it every growth step leaves its predecessor stranded.
    u8* const bytes = static_cast<u8*>(pointer);
    if (pointer != nullptr && bytes + old_size == region_.data() + region_.used() &&
        new_size > old_size && region_.remaining() >= new_size - old_size) {
        (void)region_.bump(new_size - old_size, 1);
        return pointer;
    }
    return reallocate_by_copy(pointer, old_size, new_size, alignment);
}

void ArenaAllocator::do_deallocate(void* pointer, usize size, usize alignment) noexcept {
    (void)alignment;
    // Freeing the most recent allocation winds the offset back; anything else is a no-op, which is
    // the arena's contract.
    u8* const bytes = static_cast<u8*>(pointer);
    if (bytes + size == region_.data() + region_.used()) {
        region_.rewind(region_.used() - size);
    }
}

// --- StackAllocator
// -------------------------------------------------------------------------------

StackAllocator::~StackAllocator() {
    release_block();
}

void StackAllocator::release_block() noexcept {
    if (upstream_ != nullptr && region_.data() != nullptr) {
        upstream_->deallocate(region_.data(), region_.capacity(), kDefaultAlignment);
    }
    upstream_ = nullptr;
    region_.adopt(nullptr, 0);
}

Status StackAllocator::reserve(usize bytes, Allocator& upstream) noexcept {
    release_block();
    if (bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "a stack of zero bytes allocates nothing");
    }
    void* block = upstream.allocate(bytes, kDefaultAlignment);
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "stack reservation failed");
    }
    upstream_ = &upstream;
    region_.adopt(static_cast<u8*>(block), bytes);
    return ok();
}

Status StackAllocator::reserve(usize bytes) noexcept {
    return reserve(bytes, system_allocator(domain_));
}

void StackAllocator::adopt(void* buffer, usize bytes) noexcept {
    release_block();
    region_.adopt(static_cast<u8*>(buffer), bytes);
}

void StackAllocator::release(Marker marker) noexcept {
    CY_ASSERT_MSG(marker <= region_.used(),
                  "StackAllocator::release() on a marker ahead of the current top — markers are "
                  "released in the reverse of the order they were taken");
    region_.rewind(marker);
}

void* StackAllocator::do_allocate(usize size, usize alignment) noexcept {
    return region_.bump(size, alignment);
}

void* StackAllocator::do_reallocate(void* pointer, usize old_size, usize new_size,
                                    usize alignment) noexcept {
    return reallocate_by_copy(pointer, old_size, new_size, alignment);
}

void StackAllocator::do_deallocate(void* pointer, usize size, usize alignment) noexcept {
    (void)alignment;
    u8* const bytes = static_cast<u8*>(pointer);
    if (bytes + size == region_.data() + region_.used()) {
        region_.rewind(region_.used() - size);
    }
}

}  // namespace cy
