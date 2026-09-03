// The arena that commits into a reservation. Task 2.9. Portable: the platform-specific part is
// virtual_reserve/commit/decommit/release, which the two units beside this one provide.

#include <cy/core/memory/virtual_memory.h>

#include <cy/core/memory/domain.h>

namespace cy {

VirtualArena::~VirtualArena() {
    if (base_ == nullptr) {
        return;
    }
    if (committed_ != 0) {
        domain_record_free(domain_, committed_);
    }
    domain_record_reservation(domain_, -static_cast<i64>(reserved_));
    virtual_release(base_, reserved_);
    base_ = nullptr;
}

Status VirtualArena::reserve(usize bytes) noexcept {
    if (base_ != nullptr) {
        return fail(ErrorCode::AlreadyExists, "this arena already holds a reservation");
    }
    if (bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "a reservation of zero bytes reserves nothing");
    }
    const VirtualMemoryInfo info = virtual_memory_info();
    if (!info.supported) {
        return fail(ErrorCode::Unsupported, "this platform does not reserve address space");
    }
    void* mapping = virtual_reserve(bytes);
    if (mapping == nullptr) {
        return fail(ErrorCode::OutOfMemory, "address space reservation was refused");
    }
    base_ = static_cast<u8*>(mapping);
    reserved_ = bytes;
    page_size_ = info.page_size;
    // Reserved address space is reported and does not count against a budget: it is not memory in
    // use. Only `commit_through` records an allocation.
    domain_record_reservation(domain_, static_cast<i64>(bytes));
    return ok();
}

bool VirtualArena::commit_through(usize wanted) noexcept {
    if (wanted <= committed_) {
        return true;
    }
    if (wanted > reserved_) {
        return false;
    }
    const usize target = ((wanted + page_size_ - 1) / page_size_) * page_size_;
    const usize bounded = (target > reserved_) ? reserved_ : target;
    if (!virtual_commit(base_, committed_, bounded - committed_)) {
        ++commit_failures_;
        return false;
    }
    domain_record_allocation(domain_, bounded - committed_);
    committed_ = bounded;
    return true;
}

void* VirtualArena::bump(usize size, usize alignment) noexcept {
    if (base_ == nullptr) {
        return nullptr;
    }
    const usize aligned = align_up(offset_, alignment);
    const usize end = aligned + size;
    if (end > reserved_ || !commit_through(end)) {
        return nullptr;
    }
    offset_ = end;
    high_water_ = (offset_ > high_water_) ? offset_ : high_water_;
    return base_ + aligned;
}

void VirtualArena::reset() noexcept {
    offset_ = 0;
}

void VirtualArena::decommit_all() noexcept {
    if (base_ == nullptr || committed_ == 0) {
        offset_ = 0;
        return;
    }
    if (virtual_decommit(base_, 0, committed_)) {
        domain_record_free(domain_, committed_);
        committed_ = 0;
    }
    offset_ = 0;
}

void* VirtualArena::do_allocate(usize size, usize alignment) noexcept {
    return bump(size, alignment);
}

void* VirtualArena::do_reallocate(void* pointer, usize old_size, usize new_size,
                                  usize alignment) noexcept {
    // Growing the most recent allocation in place, exactly as ArenaAllocator does: it is what a
    // container filling a reservation does, and it is the case that makes a growing cache free.
    auto* bytes = static_cast<u8*>(pointer);
    if (pointer != nullptr && bytes + old_size == base_ + offset_ && new_size > old_size) {
        const usize end = offset_ + (new_size - old_size);
        if (end <= reserved_ && commit_through(end)) {
            offset_ = end;
            high_water_ = (offset_ > high_water_) ? offset_ : high_water_;
            return pointer;
        }
    }
    return reallocate_by_copy(pointer, old_size, new_size, alignment);
}

void VirtualArena::do_deallocate(void* pointer, usize size, usize alignment) noexcept {
    (void)alignment;
    auto* bytes = static_cast<u8*>(pointer);
    if (bytes + size == base_ + offset_) {
        offset_ -= size;
    }
}

}  // namespace cy
