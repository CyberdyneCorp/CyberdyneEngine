// The general-heap measurement of task 2.10, on the engine's own allocation pattern.
//
// `core-memory-and-containers` — "General heap is an integration decided by measurement": the heap
// behind `SystemAllocator` is a proven third-party allocator selected by BENCHMARK on target
// platforms, not an engine-written implementation and not a preference; the choice stays
// replaceable behind the allocator interface, and the benchmark is part of the performance suite so
// a regression in the choice is visible.
//
// WHAT IS MEASURED, AND WHY IT IS THIS. The interesting question is not "how fast is malloc" — it
// is whether the general heap matters at all once the engine's own allocators cover the per-frame
// pattern. So this runs four workloads:
//
//   general-churn   many small blocks, allocated and freed in a random order across four threads.
//                   The pattern a general heap is actually good or bad at.
//   frame-arena     the same number of allocations, from a per-frame arena, reset each frame.
//                   What the engine does instead, and the number the heap has to be compared to.
//   pool-churn      fixed-size objects through PoolAllocator, acquired and released.
//   mixed-frame     one frame's shape: a few large blocks from the heap and thousands of small ones
//                   from the arena and the pool, repeated.
//
// The comparison against a candidate allocator is made by running this same binary under
// LD_PRELOAD, so that the two runs differ in nothing but the heap underneath. README.md records the
// numbers and the decision.

#include <cy/core/memory/arena.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/pool.h>
#include <cy/core/memory/system_allocator.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// A deterministic sequence, so two runs allocate in exactly the same order. xorshift64 rather than
/// <random>, which is neither reproducible across standard libraries nor cheap enough to stay out
/// of the measurement.
class Sequence {
public:
    explicit Sequence(cy::u64 seed) noexcept : state_(seed | 1u) {}

    cy::u64 next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }

    cy::u32 in_range(cy::u32 low, cy::u32 high) noexcept {
        return low + static_cast<cy::u32>(next() % (high - low));
    }

private:
    cy::u64 state_;
};

struct Result {
    const char* name;
    double nanoseconds_per_operation;
    cy::u64 operations;
};

/// Run `threads` copies of `worker(index)` and return the total operations they report, with the
/// wall time around the whole set.
///
/// Extracted so that the two threaded workloads state their inner loop and nothing else: the thread
/// bookkeeping is identical between them, and inlining it into both put the pair of them at the top
/// of this file's complexity report for no reason a reader benefits from.
template <class Worker>
Result run_threaded(const char* name, cy::u32 threads, Worker&& worker) {
    std::atomic<cy::u64> total_operations{0};
    const auto started = Clock::now();

    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (cy::u32 index = 0; index < threads; ++index) {
        workers.emplace_back([index, &worker, &total_operations] {
            total_operations.fetch_add(worker(index), std::memory_order_relaxed);
        });
    }
    for (std::thread& thread : workers) {
        thread.join();
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
    const cy::u64 operations = total_operations.load();
    return {name, static_cast<double>(elapsed.count()) / static_cast<double>(operations),
            operations};
}

/// How many blocks one thread keeps live at once in the churn workloads.
constexpr cy::u32 kLiveBlocks = 512;

/// One thread of the general-churn workload: small blocks in a shuffled free order.
cy::u64 churn_one_thread(cy::u32 seed_index, cy::u32 iterations) {
    cy::SystemAllocator& heap = cy::system_allocator(cy::MemoryDomain::Engine);
    Sequence sequence(0x9E3779B97F4A7C15ull ^ seed_index);
    void* live[kLiveBlocks] = {};
    cy::usize sizes[kLiveBlocks] = {};
    cy::u64 operations = 0;

    for (cy::u32 step = 0; step < iterations; ++step) {
        const cy::u32 slot = sequence.in_range(0, kLiveBlocks);
        if (live[slot] != nullptr) {
            heap.deallocate(live[slot], sizes[slot], 16);
            ++operations;
        }
        sizes[slot] = sequence.in_range(16, 512);
        live[slot] = heap.allocate(sizes[slot], 16);
        if (live[slot] != nullptr) {
            // Touch the block: an allocator that hands back cold pages is not free, and a benchmark
            // that never writes hides that.
            std::memset(live[slot], 1, 16);
        }
        ++operations;
    }
    for (cy::u32 slot = 0; slot < kLiveBlocks; ++slot) {
        if (live[slot] != nullptr) {
            heap.deallocate(live[slot], sizes[slot], 16);
            ++operations;
        }
    }
    return operations;
}

/// One thread of the frame-arena workload: the same allocations, bump-allocated and reset per
/// frame.
cy::u64 arena_one_thread(cy::u32 seed_index, cy::u32 iterations) {
    cy::ArenaAllocator arena(cy::MemoryDomain::Frame, "bench-frame");
    if (!arena.reserve(cy::usize{4} * 1024 * 1024).has_value()) {
        return 0;
    }
    Sequence sequence(0x9E3779B97F4A7C15ull ^ seed_index);
    cy::u64 operations = 0;
    for (cy::u32 step = 0; step < iterations; ++step) {
        if ((step % 512) == 0) {
            arena.reset();  // the frame boundary
        }
        if (void* block = arena.bump(sequence.in_range(16, 512), 16); block != nullptr) {
            std::memset(block, 1, 16);
        }
        ++operations;
    }
    return operations;
}

/// Small blocks in a shuffled free order, on several threads at once — the shape a general heap is
/// judged on. Sizes span the range an engine actually asks for: nodes, names, small vectors.
Result general_churn(cy::u32 threads, cy::u32 iterations) {
    return run_threaded("general-churn", threads, [iterations](cy::u32 index) {
        return churn_one_thread(index, iterations);
    });
}

/// The same number of allocations, from a per-frame arena reset each frame. This is what the engine
/// does on a hot path, and it is the figure the general heap has to be read against.
Result frame_arena(cy::u32 threads, cy::u32 iterations) {
    return run_threaded("frame-arena", threads, [iterations](cy::u32 index) {
        return arena_one_thread(index, iterations);
    });
}

struct PooledObject {
    cy::u64 fields[8] = {};
};

/// Fixed-size objects through a pool: acquire, use, release. The task-record and event pattern.
Result pool_churn(cy::u32 iterations) {
    cy::PoolAllocator<PooledObject> pool(cy::MemoryDomain::Ecs, "bench-pool", 256);
    Sequence sequence(0xDEADBEEFull);
    constexpr cy::u32 kLive = 512;
    PooledObject* live[kLive] = {};
    cy::u64 operations = 0;

    const auto started = Clock::now();
    for (cy::u32 step = 0; step < iterations; ++step) {
        const cy::u32 slot = sequence.in_range(0, kLive);
        if (live[slot] != nullptr) {
            pool.release(live[slot]);
            ++operations;
        }
        live[slot] = pool.acquire();
        if (live[slot] != nullptr) {
            live[slot]->fields[0] = step;
        }
        ++operations;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
    for (PooledObject* object : live) {
        pool.release(object);
    }
    return {"pool-churn", static_cast<double>(elapsed.count()) / static_cast<double>(operations),
            operations};
}

/// One frame's shape: a handful of large heap blocks, and thousands of small arena and pool
/// allocations. This is the number that says whether the choice of heap is visible at all.
Result mixed_frame(cy::u32 frames) {
    cy::SystemAllocator& heap = cy::system_allocator(cy::MemoryDomain::Renderer);
    cy::ArenaAllocator arena(cy::MemoryDomain::Frame, "bench-mixed");
    if (!arena.reserve(cy::usize{4} * 1024 * 1024).has_value()) {
        return {"mixed-frame", 0.0, 0};
    }
    cy::PoolAllocator<PooledObject> pool(cy::MemoryDomain::Ecs, "bench-mixed-pool", 256);
    Sequence sequence(0x1234567890ABCDEFull);
    cy::u64 operations = 0;

    const auto started = Clock::now();
    for (cy::u32 frame = 0; frame < frames; ++frame) {
        arena.reset();
        void* large[4] = {};
        cy::usize large_sizes[4] = {};
        for (cy::u32 index = 0; index < 4; ++index) {
            large_sizes[index] = sequence.in_range(64 * 1024, 512 * 1024);
            large[index] = heap.allocate(large_sizes[index], 64);
            ++operations;
        }
        PooledObject* pooled[64] = {};
        for (auto& object : pooled) {
            object = pool.acquire();
            ++operations;
        }
        for (cy::u32 index = 0; index < 2000; ++index) {
            if (void* block = arena.bump(sequence.in_range(16, 256), 16); block != nullptr) {
                std::memset(block, 1, 8);
            }
            ++operations;
        }
        for (PooledObject* object : pooled) {
            pool.release(object);
            ++operations;
        }
        for (cy::u32 index = 0; index < 4; ++index) {
            heap.deallocate(large[index], large_sizes[index], 64);
            ++operations;
        }
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
    return {"mixed-frame", static_cast<double>(elapsed.count()) / static_cast<double>(operations),
            operations};
}

void print(const Result& result) {
    std::printf("%-14s %10.2f ns/op   %12llu ops\n", result.name, result.nanoseconds_per_operation,
                static_cast<unsigned long long>(result.operations));
}

/// One command-line count. Falls back to `fallback` when the argument is not a positive number,
/// so a mistyped flag runs the default rather than measuring nothing.
cy::u32 parse_count(const char* text, cy::u32 fallback) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) {
        return fallback;
    }
    return static_cast<cy::u32>(value);
}

}  // namespace

int main(int argc, char** argv) {
    // strtoul rather than atoi: atoi cannot report a non-numeric argument, and a benchmark that
    // silently ran at scale 0 because it was handed `--help` reports "inf ns/op" and looks broken.
    const cy::u32 scale = (argc > 1) ? parse_count(argv[1], 1) : 1;
    const cy::u32 threads = (argc > 2) ? parse_count(argv[2], 4) : 4;

    std::printf("heap pattern benchmark — scale %u, %u threads\n", scale, threads);
    print(general_churn(threads, 200000 * scale));
    print(frame_arena(threads, 200000 * scale));
    print(pool_churn(400000 * scale));
    print(mixed_frame(200 * scale));

    const cy::DomainStats engine = cy::domain_stats_recursive(cy::MemoryDomain::Engine);
    std::printf("live %llu bytes, peak %llu bytes, %llu allocations total\n",
                static_cast<unsigned long long>(engine.live_bytes),
                static_cast<unsigned long long>(engine.peak_bytes),
                static_cast<unsigned long long>(engine.total_allocations));
    return 0;
}
