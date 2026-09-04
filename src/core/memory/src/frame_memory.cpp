// The thread-local frame arena and scratch stack. Task 2.7.

#include <cy/core/memory/frame_memory.h>

#include <cy/core/memory/system_allocator.h>

#include <atomic>

namespace cy {
namespace {

// The configured sizes. Atomics rather than a mutex because they are written once, at startup, and
// read by every thread that creates its arenas afterwards.
std::atomic<usize> g_frame_bytes{usize{2} * 1024 * 1024};
std::atomic<usize> g_scratch_bytes{usize{256} * 1024};

/// One thread's arenas, created on first use and destroyed when the thread exits.
///
/// A thread_local object with a destructor, rather than a pooled block that outlives the thread: an
/// arena's block is one allocation and giving it back is correct, so there is nothing here to
/// declare to the leak detector.
struct ThreadArenas {
    ArenaAllocator frame{MemoryDomain::Frame, "frame-arena"};
    StackAllocator scratch{MemoryDomain::Frame, "job-scratch"};
    u64 resets = 0;
    bool ready = false;

    void ensure() noexcept {
        if (ready) {
            return;
        }
        ready = true;  // set first: a failed reservation must not retry on every allocation
        (void)frame.reserve(g_frame_bytes.load(std::memory_order_relaxed),
                            system_allocator(MemoryDomain::Frame));
        (void)scratch.reserve(g_scratch_bytes.load(std::memory_order_relaxed),
                              system_allocator(MemoryDomain::Frame));
    }
};

thread_local ThreadArenas t_arenas;

}  // namespace

ArenaAllocator& frame_arena() noexcept {
    t_arenas.ensure();
    return t_arenas.frame;
}

StackAllocator& scratch_stack() noexcept {
    t_arenas.ensure();
    return t_arenas.scratch;
}

void configure_frame_memory(const FrameMemoryConfig& config) noexcept {
    g_frame_bytes.store(config.frame_bytes, std::memory_order_relaxed);
    g_scratch_bytes.store(config.scratch_bytes, std::memory_order_relaxed);
}

FrameMemoryConfig frame_memory_config() noexcept {
    FrameMemoryConfig config;
    config.frame_bytes = g_frame_bytes.load(std::memory_order_relaxed);
    config.scratch_bytes = g_scratch_bytes.load(std::memory_order_relaxed);
    return config;
}

void reset_frame_arena() noexcept {
    t_arenas.ensure();
    t_arenas.frame.reset();
    ++t_arenas.resets;
}

u64 frame_arena_epoch() noexcept {
    return t_arenas.resets;
}

FrameMemoryStats frame_memory_stats() noexcept {
    t_arenas.ensure();
    FrameMemoryStats stats;
    stats.frame_capacity = t_arenas.frame.capacity();
    stats.frame_used = t_arenas.frame.used();
    stats.frame_high_water = t_arenas.frame.high_water();
    stats.frame_overflows = t_arenas.frame.overflows();
    stats.scratch_capacity = t_arenas.scratch.capacity();
    stats.scratch_used = t_arenas.scratch.used();
    stats.scratch_high_water = t_arenas.scratch.high_water();
    stats.scratch_overflows = t_arenas.scratch.overflows();
    stats.resets = t_arenas.resets;
    return stats;
}

}  // namespace cy
