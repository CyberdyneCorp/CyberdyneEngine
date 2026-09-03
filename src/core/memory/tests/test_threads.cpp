// The properties that only hold across threads: a resolve concurrent with pool growth, the
// per-thread frame arenas, and the per-thread allocator scope. Tasks 2.1, 2.5 and 2.7.
//
// An integration test rather than a unit one, because the taxonomy is about cost: this starts
// threads, and a unit test has a millisecond and starts none.

#include <cy/test/test.h>

#include <cy/core/memory/domain.h>
#include <cy/core/memory/frame_memory.h>
#include <cy/core/memory/handle_pool.h>
#include <cy/core/memory/scope.h>
#include <cy/core/memory/system_allocator.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

CY_HANDLE_TAG(Node);

struct NodeResource {
    cy::u32 identifier = 0;
    cy::u64 payload[3] = {};
};

}  // namespace

CY_TEST_CASE("concurrent resolve during growth: one thread allocates while another resolves") {
    cy::HandlePool<NodeResource, NodeTag> pool(cy::MemoryDomain::Ecs, "nodes", 32);

    // A handle that exists before the growth starts. Resolving it must stay correct while the pool
    // commits chunk after chunk underneath the reader.
    auto seed = pool.create();
    CY_REQUIRE(seed.has_value());
    NodeResource* const seeded = pool.resolve(*seed);
    CY_REQUIRE(seeded != nullptr);
    seeded->identifier = 0xABCDEF;

    // The readers are held at a gate until every one of them exists, and each does a minimum number
    // of iterations before it may stop. Without both, an optimised build finishes the 4,000
    // creations before the four threads are scheduled at all, the readers see `stop` already true,
    // and the test passes having proved nothing — which is what it did in the Profile
    // configuration before this was written.
    std::atomic<cy::u32> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<cy::u64> resolves{0};
    std::atomic<cy::u64> mismatches{0};
    constexpr cy::u64 kMinimumResolves = 4096;

    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            cy::u64 local = 0;
            while (!stop.load(std::memory_order_relaxed) || local < kMinimumResolves) {
                NodeResource* resolved = pool.resolve(*seed);
                if (resolved != seeded || resolved->identifier != 0xABCDEF) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
                ++local;
            }
            resolves.fetch_add(local, std::memory_order_relaxed);
        });
    }

    while (ready.load(std::memory_order_acquire) != 4) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    for (cy::u32 index = 0; index < 4000; ++index) {
        auto created = pool.create();
        CY_REQUIRE(created.has_value());
        NodeResource* const object = pool.resolve(*created);
        CY_REQUIRE(object != nullptr);
        object->identifier = index;
    }

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) {
        reader.join();
    }

    CY_CHECK_EQ(mismatches.load(), 0u);
    CY_CHECK(resolves.load() >= 4u * kMinimumResolves);
    CY_CHECK(pool.chunk_count() > 1u);
    CY_CHECK_EQ(pool.size(), 4001u);
}

CY_TEST_CASE("frame arenas are per thread: one thread's reset does not touch another's") {
    // The threads are gated so that all four are alive at once. Without that, a thread that has
    // already exited has released its thread-local storage AND its arena's block, and the next
    // thread can legitimately be handed the same addresses — at which point "these two threads got
    // different arenas" is a claim about scheduling rather than about the arenas. It showed up
    // under ThreadSanitizer, where the threads are slow enough to run one after another.
    std::atomic<cy::u32> ready{0};
    std::atomic<bool> go{false};
    std::atomic<cy::u32> failures{0};
    std::atomic<cy::u32> distinct_buffers{0};
    constexpr cy::u32 kWorkers = 4;
    void* buffers[kWorkers] = {};

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (cy::u32 index = 0; index < kWorkers; ++index) {
        workers.emplace_back([&, index] {
            void* mine = cy::frame_arena().bump(4096, 16);
            if (mine == nullptr) {
                failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            buffers[index] = mine;
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int round = 0; round < 100; ++round) {
                cy::reset_frame_arena();
                if (cy::frame_memory_stats().frame_used != 0) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                if (cy::frame_arena().bump(1024, 16) == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
            distinct_buffers.fetch_add(1, std::memory_order_relaxed);
        });
    }

    while (ready.load(std::memory_order_acquire) != kWorkers) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }

    CY_CHECK_EQ(failures.load(), 0u);
    CY_CHECK_EQ(distinct_buffers.load(), kWorkers);
    // Every thread's arena is its own: four live threads, four different buffers.
    for (cy::u32 a = 0; a < kWorkers; ++a) {
        CY_REQUIRE(buffers[a] != nullptr);
        for (cy::u32 b = a + 1; b < kWorkers; ++b) {
            CY_CHECK_NE(buffers[a], buffers[b]);
        }
    }
}

CY_TEST_CASE("the allocator scope is per thread") {
    cy::SystemAllocator& audio = cy::system_allocator(cy::MemoryDomain::Audio);
    const cy::AllocatorScope scope(audio);
    CY_CHECK_EQ(cy::current_allocator().domain(), cy::MemoryDomain::Audio);

    std::atomic<cy::u32> wrong{0};
    std::thread worker([&] {
        // A worker inherits nothing: attributing its allocations to whatever the parent happened to
        // be doing is exactly the confusion the scope exists to prevent.
        if (cy::current_allocator().domain() != cy::MemoryDomain::Engine) {
            wrong.fetch_add(1, std::memory_order_relaxed);
        }
        if (cy::allocator_scope_depth() != 0) {
            wrong.fetch_add(1, std::memory_order_relaxed);
        }
    });
    worker.join();

    CY_CHECK_EQ(wrong.load(), 0u);
    CY_CHECK_EQ(cy::current_allocator().domain(), cy::MemoryDomain::Audio);
}

CY_TEST_CASE("domain accounting is correct under concurrent allocation") {
    const cy::u64 before = cy::domain_stats(cy::MemoryDomain::Network).live_bytes;

    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([] {
            cy::SystemAllocator& network = cy::system_allocator(cy::MemoryDomain::Network);
            for (int round = 0; round < 500; ++round) {
                void* block = network.allocate(256);
                if (block != nullptr) {
                    network.deallocate(block, 256);
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    CY_CHECK_EQ(cy::domain_stats(cy::MemoryDomain::Network).live_bytes, before);
    CY_CHECK(cy::domain_stats(cy::MemoryDomain::Network).total_allocations >= 2000u);
}
