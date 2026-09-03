#pragma once
// The allocator interface every engine allocation passes through. Task 2.1.
//
// `core-memory-and-containers` — "Allocator interface": allocate(size, align), reallocate,
// deallocate, an owning tag and a memory domain. Global new/delete are not used in engine code.
//
// FAILURE IS A NULL POINTER, NOT AN ERROR VALUE. The specification says so directly: "the allocator
// SHALL return null and the caller SHALL surface an error". So these functions return `void*` and
// the containers above them are what turn a null into
// `cy::fail(ErrorCode::OutOfMemory, ...)` — the boundary where the caller has a name for what it
// was trying to do.
//
// WHY THE VIRTUALS ARE PROTECTED. The public entry points are non-virtual, so a default argument on
// an alignment parameter means the same thing at every call site (a default argument on a virtual
// is bound statically and is a trap), and so that -Woverloaded-virtual is not tripped by every
// override. A hot path that must not pay the one indirect call left does not call `allocate` at
// all: the concrete allocators publish their own non-virtual entry points — `ArenaAllocator::bump`,
// `StackAllocator::push`, `PoolAllocator<T>::acquire`, `SlabAllocator::take`,
// `ChunkAllocator::acquire` — which are what the specification's "use the concrete type directly"
// means, and which are inline and final.
//
// WHERE ACCOUNTING HAPPENS. An allocator that owns real memory records it against its domain at the
// moment it takes it from the platform. An allocator that carves an already-recorded block —
// every bump allocator here — records nothing, because the bytes were charged once when the block
// was acquired. That is the same accounting a report wants: a frame arena costs its reservation,
// not its ten thousand temporaries.

#include <cy/core/base/assert.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/domain.h>

#include <cstddef>
#include <cstdint>
#include <new>

namespace cy {

/// A stable, human-readable owner. Always a string literal or storage that outlives the allocator:
/// it is copied nowhere, exactly like `cy::Error::message`.
using AllocationTag = const char*;

/// The alignment an allocation gets when the caller does not ask for one.
inline constexpr usize kDefaultAlignment = alignof(std::max_align_t);

/// Round `value` up to a multiple of `alignment`, which must be a power of two.
[[nodiscard]] constexpr usize align_up(usize value, usize alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

/// Round a pointer up to `alignment`. Expressed on the integer so that the arithmetic is the
/// obvious one; the round trip through uintptr_t is what the standard defines for this.
[[nodiscard]] inline void* align_up(void* pointer, usize alignment) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto aligned =
        static_cast<std::uintptr_t>(align_up(static_cast<usize>(address), alignment));
    return reinterpret_cast<void*>(aligned);
}

[[nodiscard]] constexpr bool is_power_of_two(usize value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

class Allocator {
public:
    Allocator(MemoryDomain domain, AllocationTag tag) noexcept : domain_(domain), tag_(tag) {}
    virtual ~Allocator() = default;

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;
    Allocator(Allocator&&) = delete;
    Allocator& operator=(Allocator&&) = delete;

    /// Allocate `size` bytes aligned to `alignment`. Null on failure, and on a zero size — an
    /// allocation of nothing has no address a caller may legitimately read.
    [[nodiscard]] void* allocate(usize size, usize alignment = kDefaultAlignment) noexcept {
        CY_ASSERT_MSG(is_power_of_two(alignment), "alignment must be a power of two");
        return (size == 0) ? nullptr : do_allocate(size, alignment);
    }

    /// Grow or shrink a block, preserving min(old_size, new_size) bytes. Null on failure, in which
    /// case the original block is untouched and still owned by the caller.
    [[nodiscard]] void* reallocate(void* pointer, usize old_size, usize new_size,
                                   usize alignment = kDefaultAlignment) noexcept {
        CY_ASSERT_MSG(is_power_of_two(alignment), "alignment must be a power of two");
        return do_reallocate(pointer, old_size, new_size, alignment);
    }

    /// Release a block. `size` and `alignment` must be the ones it was allocated with: the
    /// allocators here are sized-free, which is what lets a pool return a block to its free list
    /// without a per-allocation header. A null pointer is accepted and does nothing.
    void deallocate(void* pointer, usize size, usize alignment = kDefaultAlignment) noexcept {
        if (pointer != nullptr) {
            do_deallocate(pointer, size, alignment);
        }
    }

    [[nodiscard]] MemoryDomain domain() const noexcept { return domain_; }
    [[nodiscard]] AllocationTag tag() const noexcept { return tag_; }

    /// Whether this allocator reserves address space it has not committed. Declared per allocator
    /// rather than assumed, because reserved space is reported separately and never counts against
    /// a budget — see `virtual_memory.h`.
    [[nodiscard]] virtual bool reserves_address_space() const noexcept { return false; }

protected:
    /// `size` is never zero and `alignment` is always a power of two: the public wrappers have
    /// already established both.
    [[nodiscard]] virtual void* do_allocate(usize size, usize alignment) noexcept = 0;
    [[nodiscard]] virtual void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                              usize alignment) noexcept = 0;
    virtual void do_deallocate(void* pointer, usize size, usize alignment) noexcept = 0;

    /// The generic reallocate: allocate, copy, free. Every allocator that cannot do better than
    /// this calls it, rather than each writing the same six lines.
    [[nodiscard]] void* reallocate_by_copy(void* pointer, usize old_size, usize new_size,
                                           usize alignment) noexcept;

    MemoryDomain domain_;
    AllocationTag tag_;
};

/// Construct one `T` in `storage`, which must be suitably sized and aligned. Written here so that
/// the containers do not each spell out the placement-new incantation, and so `-fno-exceptions`
/// means the call cannot fail once it starts.
template <class T, class... Args>
T* construct_at(void* storage, Args&&... args) noexcept {
    return ::new (storage) T(static_cast<Args&&>(args)...);
}

}  // namespace cy
