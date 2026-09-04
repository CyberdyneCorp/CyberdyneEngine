#pragma once
// Buffer components: a variable-length array per entity, inline up to a capacity then heap. Part of
// task 2.2.
//
// `ecs-core`'s component table names this kind and the storage it requires. The shape below is the
// one that keeps the rest of the ECS honest:
//
//   * the column entry is a fixed-size header followed by the inline elements, so a buffer
//     component is an ordinary chunk column and archetype iteration does not learn a special case;
//   * the header is trivially relocatable — a size, two capacities and a pointer — so chunk
//     compaction still moves a row with `memcpy`, which is the property every other component has
//     to have as well;
//   * spilling to the heap is the only thing a buffer owns outside the chunk, and it is released
//     through the `ComponentReleaseFn` the archetype calls when a row goes away. That hook exists
//     for this kind and no other.
//
// THE ALLOCATOR IS THE WORLD'S, PASSED IN, NOT CAPTURED. A spill goes to the allocator the world
// was built with, so it lands under `MemoryDomain::Ecs` in M1's budget tree with everything else
// the ECS holds. Storing an `Allocator*` in the header would make the ECS's memory attributable to
// wherever the first `push_back` happened to run.

#include <cy/core/base/assert.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>

#include <algorithm>
#include <cstring>

namespace cy::ecs {

/// The fixed part of a buffer component's column entry. The inline elements follow it immediately.
struct BufferHeader {
    u32 size = 0;
    /// Elements the heap block holds. Zero while the buffer is still inline.
    u32 heap_capacity = 0;
    void* heap = nullptr;
};

static_assert(sizeof(BufferHeader) == 16, "the column entry's layout is computed from this");

/// One entity's buffer, as the caller sees it. A view: it holds the header, not a copy of the data.
///
/// Growth is fallible and says so — under -fno-exceptions there is no other way for a `push_back`
/// to report that the allocator refused, and `[[nodiscard]]` makes ignoring one a build error.
template <class T>
class BufferView {
public:
    BufferView(BufferHeader* header, u32 inline_capacity, Allocator& allocator) noexcept
        : header_(header), inline_capacity_(inline_capacity), allocator_(&allocator) {}

    [[nodiscard]] u32 size() const noexcept { return header_->size; }
    [[nodiscard]] bool empty() const noexcept { return header_->size == 0; }
    [[nodiscard]] u32 capacity() const noexcept {
        return (header_->heap != nullptr) ? header_->heap_capacity : inline_capacity_;
    }

    [[nodiscard]] T* data() noexcept {
        return (header_->heap != nullptr) ? static_cast<T*>(header_->heap) : inline_data();
    }
    [[nodiscard]] const T* data() const noexcept {
        return (header_->heap != nullptr) ? static_cast<const T*>(header_->heap) : inline_data();
    }

    [[nodiscard]] Span<T> span() noexcept { return Span<T>(data(), header_->size); }
    [[nodiscard]] Span<const T> span() const noexcept {
        return Span<const T>(data(), header_->size);
    }

    [[nodiscard]] T& operator[](u32 index) noexcept {
        CY_ASSERT_MSG(index < header_->size, "buffer index out of range");
        return data()[index];
    }
    [[nodiscard]] const T& operator[](u32 index) const noexcept {
        CY_ASSERT_MSG(index < header_->size, "buffer index out of range");
        return data()[index];
    }

    [[nodiscard]] Status push_back(const T& value) noexcept {
        if (Status room = reserve(header_->size + 1); !room) {
            return room;
        }
        data()[header_->size++] = value;
        return ok();
    }

    /// O(1) removal by moving the last element into the gap, matching `Array::remove_unordered`.
    /// A buffer component's order is the caller's business; where the ECS itself uses one —
    /// `Children` — the order is already unspecified by `ecs-core`.
    void remove_unordered(u32 index) noexcept {
        CY_ASSERT_MSG(index < header_->size, "buffer removal past the end");
        if (index >= header_->size) {
            return;
        }
        const u32 last = --header_->size;
        if (index != last) {
            data()[index] = data()[last];
        }
    }

    /// Remove the first element equal to `value`. True when one was found.
    [[nodiscard]] bool remove_first(const T& value) noexcept {
        for (u32 index = 0; index < header_->size; ++index) {
            if (data()[index] == value) {
                remove_unordered(index);
                return true;
            }
        }
        return false;
    }

    void clear() noexcept { header_->size = 0; }

    [[nodiscard]] Status reserve(u32 wanted) noexcept;

    [[nodiscard]] T* begin() noexcept { return data(); }
    [[nodiscard]] T* end() noexcept { return data() + header_->size; }
    [[nodiscard]] const T* begin() const noexcept { return data(); }
    [[nodiscard]] const T* end() const noexcept { return data() + header_->size; }

private:
    // NOLINTBEGIN(bugprone-casting-through-void) — through void* on purpose. The inline elements
    // start immediately after the header, at an offset the column layout aligned; a direct
    // reinterpret_cast from `u8*` is what -Wcast-align reports and the engine builds with -Werror.
    [[nodiscard]] T* inline_data() noexcept {
        return static_cast<T*>(
            static_cast<void*>(reinterpret_cast<u8*>(header_) + inline_offset()));
    }
    [[nodiscard]] const T* inline_data() const noexcept {
        return static_cast<const T*>(
            static_cast<const void*>(reinterpret_cast<const u8*>(header_) + inline_offset()));
    }
    // NOLINTEND(bugprone-casting-through-void)

    [[nodiscard]] static constexpr usize inline_offset() noexcept {
        return align_up(sizeof(BufferHeader), alignof(T));
    }

    BufferHeader* header_;
    u32 inline_capacity_;
    Allocator* allocator_;
};

/// The bytes one entry of a buffer column occupies: the header, the alignment padding before the
/// inline elements, and the inline elements themselves.
[[nodiscard]] constexpr u32 buffer_entry_size(u32 element_size, u32 element_alignment,
                                              u32 inline_capacity) noexcept {
    const usize offset = align_up(sizeof(BufferHeader), element_alignment);
    return static_cast<u32>(offset + (usize{element_size} * inline_capacity));
}

[[nodiscard]] constexpr u32 buffer_entry_alignment(u32 element_alignment) noexcept {
    return static_cast<u32>((element_alignment > alignof(BufferHeader)) ? element_alignment
                                                                        : alignof(BufferHeader));
}

/// Release a buffer's heap block. The `ComponentReleaseFn` a buffer component registers; the
/// element size is baked in by the template, which is what lets the hook be a plain function
/// pointer with no per-component state behind it.
template <class T>
void release_buffer(void* element, Allocator& allocator) noexcept {
    auto* header = static_cast<BufferHeader*>(element);
    if (header->heap != nullptr) {
        allocator.deallocate(header->heap, usize{header->heap_capacity} * sizeof(T), alignof(T));
        header->heap = nullptr;
        header->heap_capacity = 0;
    }
    header->size = 0;
}

template <class T>
Status BufferView<T>::reserve(u32 wanted) noexcept {
    if (wanted <= capacity()) {
        return ok();
    }
    // Doubling from the inline capacity, with a floor, so a buffer that outgrows its inline storage
    // does not then reallocate on every subsequent push.
    u32 target =
        (header_->heap_capacity == 0) ? (inline_capacity_ * 2) : (header_->heap_capacity * 2);
    target = std::max<u32>(target, 4);
    while (target < wanted) {
        target *= 2;
    }

    void* block = allocator_->allocate(usize{target} * sizeof(T), alignof(T));
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "a buffer component could not grow");
    }
    if (header_->size != 0) {
        std::memcpy(block, static_cast<const void*>(data()), usize{header_->size} * sizeof(T));
    }
    if (header_->heap != nullptr) {
        allocator_->deallocate(header_->heap, usize{header_->heap_capacity} * sizeof(T),
                               alignof(T));
    }
    header_->heap = block;
    header_->heap_capacity = target;
    return ok();
}

}  // namespace cy::ecs
