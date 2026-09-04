#pragma once
// Handle pools: chunked, address-stable slots behind a generational handle. Task 2.5.
//
// `core-memory-and-containers` — "Handle pools": `HandlePool<T>` backs every server's object
// storage — chunked, address-stable slots with generation counters, a free list, and optional
// thread-safe allocation. Chunk size is configurable per pool, and chunk pointer arrays are
// pre-reserved in thread-safe pools so growth never reallocates the pointer array under a
// concurrent reader.
//
// THE GENERATION SCHEME IS NOT REIMPLEMENTED HERE. `cy::GenerationTable` (core-values, task 1.3.2)
// owns the slot indices, the counters and the validity test; this file composes it with storage.
// The comment at the top of `<cy/core/values/handle.h>` says the same thing from the other side.
// Writing a second generation scheme would give the engine two answers to "is this handle stale",
// which is one more than a correct program can have.
//
// WHAT MAKES A RESOLVE SAFE DURING GROWTH. The chunk directory is allocated once, at its full size,
// and never reallocated. A new chunk is stored into it and then the chunk count is published with a
// release store; a reader loads the count with acquire and therefore either sees a fully
// constructed chunk or does not see it at all. Nothing else is synchronised, and `resolve()` takes
// no lock — it is what a server calls before every dereference.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/values/handle.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace cy {

/// The most chunks one pool addresses. The directory is this many pointers, taken once.
inline constexpr u32 kMaxHandlePoolChunks = 4096;

template <class T, class Tag>
class HandlePool {
public:
    using HandleType = Handle<Tag>;

    /// `objects_per_chunk` is rounded up to a power of two, so the slot-to-chunk arithmetic is a
    /// shift and a mask rather than two divisions on the resolve path.
    HandlePool(MemoryDomain domain, AllocationTag tag, u32 objects_per_chunk = 256,
               bool thread_safe = true) noexcept
        : allocator_(&system_allocator(domain)),
          tag_(tag),
          generations_(round_up_pow2(objects_per_chunk)),
          objects_per_chunk_(round_up_pow2(objects_per_chunk)),
          chunk_shift_(shift_for(round_up_pow2(objects_per_chunk))),
          chunk_mask_(round_up_pow2(objects_per_chunk) - 1),
          thread_safe_(thread_safe) {}

    ~HandlePool() { release(); }

    HandlePool(const HandlePool&) = delete;
    HandlePool& operator=(const HandlePool&) = delete;

    void set_allocator(Allocator& allocator) noexcept {
        CY_ASSERT_MSG(chunk_count_.load(std::memory_order_relaxed) == 0,
                      "the allocator is chosen before the first slot");
        allocator_ = &allocator;
    }

    /// Construct an object and return a handle to it.
    template <class... Args>
    [[nodiscard]] Expected<HandleType, Error> create(Args&&... args) noexcept {
        const Lock guard(mutex_, thread_safe_);

        const Expected<u32, Error> slot = generations_.allocate();
        if (!slot) {
            return make_unexpected(slot.error());
        }
        T* storage = commit_slot(*slot);
        if (storage == nullptr) {
            (void)generations_.release(*slot);
            return fail(ErrorCode::OutOfMemory, "handle pool could not commit a chunk");
        }
        construct_at<T>(storage, std::forward<Args>(args)...);
        live_.fetch_add(1, std::memory_order_relaxed);
        return HandleType::from_slot(*slot, generations_.generation_of(*slot));
    }

    /// THE HOT PATH. The object, or null when the handle is stale or was never issued. Lock-free,
    /// allocates nothing, and emits nothing.
    [[nodiscard]] T* resolve(HandleType handle) noexcept {
        if (!generations_.is_live(handle)) {
            return nullptr;
        }
        return slot_pointer(handle.index());
    }
    [[nodiscard]] const T* resolve(HandleType handle) const noexcept {
        if (!generations_.is_live(handle)) {
            return nullptr;
        }
        return slot_pointer(handle.index());
    }

    /// Destroy the object and free its slot. Every handle to it becomes stale.
    Status destroy(HandleType handle) noexcept {
        const Lock guard(mutex_, thread_safe_);

        if (!generations_.is_live(handle)) {
            return fail(ErrorCode::NotFound, "handle is stale or was never allocated");
        }
        if (T* object = slot_pointer(handle.index()); object != nullptr) {
            object->~T();
        }
        live_.fetch_sub(1, std::memory_order_relaxed);
        return generations_.release(handle.index());
    }

    [[nodiscard]] u32 size() const noexcept { return live_.load(std::memory_order_relaxed); }
    [[nodiscard]] u32 capacity() const noexcept {
        return chunk_count_.load(std::memory_order_acquire) * objects_per_chunk_;
    }
    [[nodiscard]] u32 chunk_count() const noexcept {
        return chunk_count_.load(std::memory_order_acquire);
    }
    [[nodiscard]] u32 objects_per_chunk() const noexcept { return objects_per_chunk_; }
    [[nodiscard]] AllocationTag tag() const noexcept { return tag_; }
    [[nodiscard]] u64 committed_bytes() const noexcept {
        return static_cast<u64>(capacity()) * sizeof(T);
    }
    /// How many handles this pool has rejected as stale — the counter that says a caller is holding
    /// a reference across a destruction.
    [[nodiscard]] u64 stale_rejections() const noexcept { return generations_.stale_rejections(); }

private:
    /// A mutex held only when the pool was asked to be thread-safe. A branch on a bool rather than
    /// two class templates, because the alternative is two instantiations of everything above to
    /// avoid one predictable branch on a path that is already allocating.
    class Lock {
    public:
        Lock(std::mutex& mutex, bool engaged) noexcept : mutex_(mutex), engaged_(engaged) {
            if (engaged_) {
                mutex_.lock();
            }
        }
        ~Lock() {
            if (engaged_) {
                mutex_.unlock();
            }
        }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

    private:
        std::mutex& mutex_;
        bool engaged_;
    };

    [[nodiscard]] static constexpr u32 round_up_pow2(u32 value) noexcept {
        u32 result = 1;
        while (result < value && result < (1u << 20)) {
            result <<= 1;
        }
        return result;
    }

    [[nodiscard]] static constexpr u32 shift_for(u32 power_of_two) noexcept {
        u32 shift = 0;
        while ((1u << shift) < power_of_two) {
            ++shift;
        }
        return shift;
    }

    /// The directory, taken once at its full size. Null when it could not be taken, which makes the
    /// first create fail rather than leaving a half-built pool.
    [[nodiscard]] bool ensure_directory() noexcept {
        if (chunks_ != nullptr) {
            return true;
        }
        void* block = allocator_->allocate(kMaxHandlePoolChunks * sizeof(T*), alignof(T*));
        if (block == nullptr) {
            return false;
        }
        chunks_ = static_cast<T**>(block);
        for (u32 index = 0; index < kMaxHandlePoolChunks; ++index) {
            chunks_[index] = nullptr;
        }
        return true;
    }

    /// Make sure the chunk holding `slot` exists, and return the storage for it. Called under the
    /// lock; the release store at the end is what a concurrent `resolve()` acquires.
    [[nodiscard]] T* commit_slot(u32 slot) noexcept {
        const u32 chunk = slot >> chunk_shift_;
        if (chunk >= kMaxHandlePoolChunks || !ensure_directory()) {
            return nullptr;
        }
        if (chunk >= chunk_count_.load(std::memory_order_relaxed)) {
            void* block = allocator_->allocate(static_cast<usize>(objects_per_chunk_) * sizeof(T),
                                               alignof(T));
            if (block == nullptr) {
                return nullptr;
            }
            chunks_[chunk] = static_cast<T*>(block);
            // Publish. Everything above — the directory, the chunk's own allocation — happened
            // before this store, so a reader that acquires the count sees all of it.
            chunk_count_.store(chunk + 1, std::memory_order_release);
        }
        return chunks_[chunk] + (slot & chunk_mask_);
    }

    [[nodiscard]] T* slot_pointer(u32 slot) const noexcept {
        const u32 chunk = slot >> chunk_shift_;
        if (chunk >= chunk_count_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        return chunks_[chunk] + (slot & chunk_mask_);
    }

    void release() noexcept {
        const u32 chunks = chunk_count_.load(std::memory_order_acquire);
        for (u32 chunk = 0; chunk < chunks; ++chunk) {
            // Live objects are found through the generation table rather than by walking storage:
            // a slot that was never allocated holds no object, and `generation_of` answers zero for
            // exactly those, so a destructor is never run over uninitialised memory.
            for (u32 offset = 0; offset < objects_per_chunk_; ++offset) {
                const u32 slot = (chunk << chunk_shift_) + offset;
                if (generations_.generation_of(slot) != 0) {
                    chunks_[chunk][offset].~T();
                }
            }
            allocator_->deallocate(chunks_[chunk],
                                   static_cast<usize>(objects_per_chunk_) * sizeof(T), alignof(T));
        }
        if (chunks_ != nullptr) {
            allocator_->deallocate(static_cast<void*>(chunks_), kMaxHandlePoolChunks * sizeof(T*),
                                   alignof(T*));
            chunks_ = nullptr;
        }
        chunk_count_.store(0, std::memory_order_release);
        live_.store(0, std::memory_order_relaxed);
    }

    mutable std::mutex mutex_;
    Allocator* allocator_;
    AllocationTag tag_;
    GenerationTable generations_;
    T** chunks_ = nullptr;  // pre-reserved at kMaxHandlePoolChunks, never reallocated
    std::atomic<u32> chunk_count_{0};
    u32 objects_per_chunk_;
    u32 chunk_shift_;
    u32 chunk_mask_;
    // Relaxed: it is written under the lock and read without one, so `size()` is a snapshot rather
    // than a synchronised figure — which is what a report wants and what keeps `resolve()` free of
    // the lock entirely.
    std::atomic<u32> live_{0};
    bool thread_safe_;
};

}  // namespace cy
