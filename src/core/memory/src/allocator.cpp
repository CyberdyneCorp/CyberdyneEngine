// The parts of the allocator interface that are not inline. Task 2.1.

#include <cy/core/memory/allocator.h>

#include <cstring>

namespace cy {

void* Allocator::reallocate_by_copy(void* pointer, usize old_size, usize new_size,
                                    usize alignment) noexcept {
    if (new_size == 0) {
        deallocate(pointer, old_size, alignment);
        return nullptr;
    }
    void* fresh = allocate(new_size, alignment);
    if (fresh == nullptr) {
        return nullptr;  // the original block is untouched and still the caller's
    }
    if (pointer != nullptr) {
        std::memcpy(fresh, pointer, (old_size < new_size) ? old_size : new_size);
        deallocate(pointer, old_size, alignment);
    }
    return fresh;
}

}  // namespace cy
