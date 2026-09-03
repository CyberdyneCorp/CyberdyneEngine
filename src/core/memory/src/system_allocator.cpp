// The platform heap, and the per-domain instances above it. Tasks 2.1 and 2.10.
//
// `std::aligned_alloc` is C++17 and requires the size to be a multiple of the alignment; MSVC does
// not provide it at all and wants `_aligned_malloc`/`_aligned_free`, which is why the two spellings
// are separated here rather than at every call site. This is the one platform conditional in the
// module that is not a whole translation unit of its own: it is three lines and no behaviour
// differs between the branches, so splitting the file would hide the symmetry rather than show it.

#include <cy/core/memory/system_allocator.h>

#include <cstdlib>
#include <cstring>

namespace cy {
namespace {

[[nodiscard]] void* heap_allocate(usize size, usize alignment) noexcept {
#if defined(_MSC_VER)
    return _aligned_malloc(size, alignment);
#else
    // aligned_alloc requires a size that is a multiple of the alignment. Rounding up here rather
    // than asking the caller to is the whole of the difference between the two platforms.
    return std::aligned_alloc(alignment, align_up(size, alignment));
#endif
}

void heap_free(void* pointer) noexcept {
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

}  // namespace

void* SystemAllocator::do_allocate(usize size, usize alignment) noexcept {
    void* pointer = heap_allocate(size, alignment);
    if (pointer == nullptr) {
        return nullptr;
    }
    live_bytes_.fetch_add(size, std::memory_order_relaxed);
    live_allocations_.fetch_add(1, std::memory_order_relaxed);
    domain_record_allocation(domain_, size);
    return pointer;
}

void* SystemAllocator::do_reallocate(void* pointer, usize old_size, usize new_size,
                                     usize alignment) noexcept {
    // std::realloc does not preserve an over-aligned block's alignment, so there is nothing to
    // gain by calling it: the copy path is what a correct aligned reallocation is.
    return reallocate_by_copy(pointer, old_size, new_size, alignment);
}

void SystemAllocator::do_deallocate(void* pointer, usize size, usize alignment) noexcept {
    (void)alignment;
    heap_free(pointer);
    live_bytes_.fetch_sub(size, std::memory_order_relaxed);
    live_allocations_.fetch_sub(1, std::memory_order_relaxed);
    domain_record_free(domain_, size);
}

u64 SystemAllocator::live_bytes() const noexcept {
    return live_bytes_.load(std::memory_order_relaxed);
}

u64 SystemAllocator::live_allocations() const noexcept {
    return live_allocations_.load(std::memory_order_relaxed);
}

SystemAllocator& default_allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

SystemAllocator& system_allocator(MemoryDomain domain) noexcept {
    // One instance per domain, constructed on first use; a function-local static is initialised
    // thread-safely by the standard. Their destructors run at exit and do nothing — a system
    // allocator holds no memory of its own, it only counts — so a caller that frees a block during
    // static destruction, after this array has been destroyed, is still calling a live object's
    // code on live counters. That is the property that matters here, not the destruction order.
    static SystemAllocator instances[kMemoryDomainCount] = {
        {MemoryDomain::Engine, "engine"},
        {MemoryDomain::Ecs, "ecs"},
        {MemoryDomain::Frame, "frame"},
        {MemoryDomain::Renderer, "renderer"},
        {MemoryDomain::Gpu, "gpu"},
        {MemoryDomain::Physics, "physics"},
        {MemoryDomain::Animation, "animation"},
        {MemoryDomain::Audio, "audio"},
        {MemoryDomain::Assets, "assets"},
        {MemoryDomain::Streaming, "streaming"},
        {MemoryDomain::World, "world"},
        {MemoryDomain::Network, "network"},
        {MemoryDomain::Scripting, "scripting"},
        {MemoryDomain::Editor, "editor"},
    };
    const u32 index = static_cast<u32>(domain);
    return instances[(index < kMemoryDomainCount) ? index : 0];
}

}  // namespace cy
