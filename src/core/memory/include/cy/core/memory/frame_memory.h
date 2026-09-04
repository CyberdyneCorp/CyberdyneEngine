#pragma once
// Per-thread frame memory and per-job scratch. Task 2.7.
//
// `core-memory-and-containers` — "Scratch and frame memory": a per-thread frame arena and a per-job
// scratch arena; memory from them does not outlive the frame or the job respectively; and
// development builds poison the memory on reset so a use-after-reset is obvious.
//
// PER THREAD, NOT PER PROCESS. Both arenas are thread-local, so allocating from one takes no lock
// and a worker's temporaries never touch another worker's cache lines. `reset_frame_arenas()` at a
// frame boundary is therefore a per-thread call: the thread that ends the frame resets its own, and
// each worker resets its own when it is next told to. `frame_arena_epoch()` is what lets a worker
// notice that it has not.
//
// THE JOB SCRATCH IS A STACK. A job takes a `ScratchScope`, allocates, and the scope releases
// everything on the way out — including on an early return, which is the case a manual release
// misses. Nested jobs nest their scopes, and the LIFO order is the one a call stack already has.

#include <cy/core/base/expected.h>
#include <cy/core/memory/arena.h>

namespace cy {

/// This thread's frame arena. Sized by `configure_frame_memory` on first use.
[[nodiscard]] ArenaAllocator& frame_arena() noexcept;

/// This thread's scratch stack.
[[nodiscard]] StackAllocator& scratch_stack() noexcept;

/// The sizes each thread's arenas are created with. Set before the threads start; a thread that has
/// already created its arenas keeps the size it was given.
struct FrameMemoryConfig {
    usize frame_bytes = usize{2} * 1024 * 1024;
    usize scratch_bytes = usize{256} * 1024;
};

void configure_frame_memory(const FrameMemoryConfig& config) noexcept;
[[nodiscard]] FrameMemoryConfig frame_memory_config() noexcept;

/// Reset THIS THREAD's frame arena, in O(1), poisoning it in development builds. Called at the
/// frame boundary by every thread that allocated during the frame.
void reset_frame_arena() noexcept;

/// The number of times this thread has reset its frame arena. A worker comparing this against the
/// frame index is how "a thread that never reset" is detected rather than assumed.
[[nodiscard]] u64 frame_arena_epoch() noexcept;

/// What this thread's frame arena has cost and how close it came to running out.
struct FrameMemoryStats {
    usize frame_capacity = 0;
    usize frame_used = 0;
    usize frame_high_water = 0;
    u64 frame_overflows = 0;
    usize scratch_capacity = 0;
    usize scratch_used = 0;
    usize scratch_high_water = 0;
    u64 scratch_overflows = 0;
    u64 resets = 0;
};

[[nodiscard]] FrameMemoryStats frame_memory_stats() noexcept;

/// A scratch scope: everything allocated inside it is released when it ends.
///
///   void run(Job& job) {
///       cy::ScratchScope scratch;
///       auto* buffer = scratch.allocate<f32>(count);
///       ...
///   }                                  // released here, on every path out
class ScratchScope {
public:
    ScratchScope() noexcept : stack_(&scratch_stack()), marker_(stack_->mark()) {}
    ~ScratchScope() { stack_->release(marker_); }

    ScratchScope(const ScratchScope&) = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;
    ScratchScope(ScratchScope&&) = delete;
    ScratchScope& operator=(ScratchScope&&) = delete;

    /// Uninitialised storage for `count` objects of `T`, or null. No constructor is run: this is
    /// scratch for trivially destructible data, and a caller that needs construction places it.
    template <class T>
    [[nodiscard]] T* allocate(usize count) noexcept {
        return static_cast<T*>(stack_->push(count * sizeof(T), alignof(T)));
    }

    [[nodiscard]] StackAllocator& allocator() const noexcept { return *stack_; }

private:
    StackAllocator* stack_;
    StackAllocator::Marker marker_;
};

}  // namespace cy
