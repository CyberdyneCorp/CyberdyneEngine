// Deterministic scheduling and deterministic parallel primitives. Task 3.2.10.
//
// M9 validates determinism across a whole simulation; M1's job is to make that possible, and these
// cases are what "possible" means concretely. A deterministic run's execution order is a function of
// submission sequence and nothing else; a parallel reduction's combination order is a function of
// the partitioning and nothing else; the single-threaded mode produces the same answer as the
// parallel one, so a divergence between them localises a scheduling-dependent defect.
//
// What is deliberately NOT claimed here: that the engine is deterministic. Nothing above this layer
// exists yet. What is claimed is that the mechanisms are correct in shape, which is what M9 needs
// to have been seeded.

#include "harness.h"

#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/parallel.h>
#include <cy/core/jobs/sync.h>

#include <atomic>

namespace {

using namespace cy;
using namespace cy::jobs;
using cy::jobs::test::ScopedJobSystem;

struct Trace {
    static constexpr u32 kCapacity = 512;
    std::atomic<u32> count{0};
    u32 order[kCapacity] = {};

    void note(u32 value) noexcept {
        const u32 index = count.fetch_add(1, std::memory_order_relaxed);
        if (index < kCapacity) {
            order[index] = value;
        }
    }
};

/// One run of a fixed workload. `completed` is set once every task has noted itself, so the calling
/// thread can block on it rather than helping — see the test below for why that matters.
void run_workload(JobSystem& jobs, Trace& trace, Event* completed, u32 count) {
    struct Item {
        Trace* trace;
        u32 index;
        Event* completed;
        u32 total;
    };
    static Item items[64];

    JobHandle handles[64];
    for (u32 i = 0; i < count; ++i) {
        items[i].trace = &trace;
        items[i].index = i;
        items[i].completed = completed;
        items[i].total = count;
        JobDesc desc;
        desc.name = "ordered";
        desc.user = &items[i];
        // Deliberately varied, so that a mode which honoured priorities would produce a different
        // order and this test would catch it.
        desc.priority = i % 3 == 0 ? Priority::Background : Priority::High;
        desc.deadline = Deadline{i + 1, 0};
        desc.body = [](const TaskContext&, void* user) noexcept {
            auto* item = static_cast<Item*>(user);
            item->trace->note(item->index);
            if (item->completed != nullptr &&
                item->trace->count.load(std::memory_order_relaxed) == item->total) {
                item->completed->set();
            }
        };
        auto handle = jobs.submit(desc);
        if (handle) {
            handles[i] = handle.value();
        }
    }

    if (completed != nullptr) {
        // Block, rather than wait: `wait()` runs ready tasks on this thread, which would make the
        // calling thread a second executor and defeat the point of asking for one worker.
        completed->wait();
    }
    jobs.wait_all(handles, count);
}

}  // namespace

CY_TEST_CASE("a deterministic run takes its tasks in submission order") {
    // ONE worker, and the calling thread deliberately blocked rather than helping, so that exactly
    // one thread takes from the ready set. That is what makes the observation exact.
    //
    // With several workers the *taking* is still in submission order — the ready set is a min-heap
    // on the submission sequence and nothing else — but what a test can observe is when each body
    // began, and a thread descheduled between being handed a task and running its first instruction
    // reorders that by an unbounded amount. The scheduler's ordering is not a property a wall clock
    // can measure across threads, so the case that measures it uses one.
    JobSystemConfig config;
    config.worker_count = 1;
    config.mode = SchedulingMode::Deterministic;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());
    CY_CHECK_EQ(system->mode(), SchedulingMode::Deterministic);

    Trace trace;
    Event completed;
    run_workload(system.get(), trace, &completed, 64);
    CY_REQUIRE_EQ(trace.count.load(), 64u);

    // Submission order exactly. Priorities and deadline hints are ignored: "deadlines and
    // priorities influence when work runs, never what the simulation computes", and in
    // deterministic mode they do not influence even that.
    for (u32 i = 0; i < 64; ++i) {
        CY_REQUIRE_EQ(trace.order[i], i);
    }
}

CY_TEST_CASE("determinism does not require single-threaded execution") {
    // The other half of the same requirement: the fixed order is honoured across however many
    // workers are running, and every task still runs exactly once. What is deliberately NOT
    // asserted is the interleaving — see the case above.
    JobSystemConfig config;
    config.worker_count = 4;
    config.mode = SchedulingMode::Deterministic;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());

    Trace trace;
    run_workload(system.get(), trace, nullptr, 64);
    CY_REQUIRE_EQ(trace.count.load(), 64u);

    bool seen[64] = {};
    for (u32 i = 0; i < 64; ++i) {
        CY_REQUIRE(trace.order[i] < 64u);
        CY_REQUIRE_FALSE(seen[trace.order[i]]);
        seen[trace.order[i]] = true;
    }
}

CY_TEST_CASE("single-threaded deterministic mode runs the exact submission order") {
    JobSystemConfig config;
    config.worker_count = 8;  // asked for, and deliberately not honoured
    config.mode = SchedulingMode::DeterministicSingleThreaded;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());
    CY_CHECK_EQ(system->worker_count(), 0u);

    Trace trace;
    run_workload(system.get(), trace, nullptr, 64);
    CY_REQUIRE_EQ(trace.count.load(), 64u);

    // With no workers at all the calling thread runs everything, in the ready set's order. That is
    // what makes the mode useful for localising a scheduling-dependent defect.
    for (u32 i = 0; i < 64; ++i) {
        CY_REQUIRE_EQ(trace.order[i], i);
    }
}

CY_TEST_CASE("a floating-point reduction is bit-identical across runs and worker counts") {
    // The specification's scenario: "a parallel floating-point reduction runs twice with different
    // worker timing THEN it SHALL produce bit-identical results". Floating-point addition is not
    // associative, so this is a property of the combination order rather than of the arithmetic.
    constexpr u64 kCount = 20'000;

    auto map = [](u64 index) noexcept {
        // Values chosen so that a different summation order gives a different last bit: a large
        // running total with small increments loses the increments.
        return index % 7 == 0 ? 1.0e8 : 1.0e-3;
    };
    auto combine = [](f64 a, f64 b) noexcept { return a + b; };

    f64 first = 0.0;
    {
        ScopedJobSystem system(1);
        CY_REQUIRE(system.started());
        auto result = parallel_reduce(system.get(), kCount, 64, 0.0, map, combine, "sum");
        CY_REQUIRE(result.has_value());
        first = result.value();
    }

    for (u32 workers = 2; workers <= 6; workers += 2) {
        ScopedJobSystem system(workers);
        CY_REQUIRE(system.started());
        auto result = parallel_reduce(system.get(), kCount, 64, 0.0, map, combine, "sum");
        CY_REQUIRE(result.has_value());
        // Bit-identical, not approximately equal. An approximate check here would pass for a
        // reduction that folded in completion order, which is the defect being excluded.
        CY_CHECK_EQ(result.value(), first);
    }
}

CY_TEST_CASE("an exclusive scan is deterministic and correct") {
    ScopedJobSystem system(4);
    CY_REQUIRE(system.started());

    constexpr u64 kCount = 5000;
    static u64 input[kCount];
    static u64 output[kCount];
    for (u64 i = 0; i < kCount; ++i) {
        input[i] = i % 13;
        output[i] = 0xFFFF'FFFF'FFFF'FFFFull;
    }

    auto combine = [](u64 a, u64 b) noexcept { return a + b; };
    CY_REQUIRE(
        parallel_exclusive_scan(system.get(), input, output, kCount, 64, u64{0}, combine, "scan")
            .has_value());

    u64 running = 0;
    for (u64 i = 0; i < kCount; ++i) {
        CY_REQUIRE_EQ(output[i], running);
        running += input[i];
    }
}

CY_TEST_CASE("a parallel sort produces the same array every run") {
    ScopedJobSystem system(4);
    CY_REQUIRE(system.started());

    constexpr u64 kCount = 4000;
    static u32 data[kCount];
    static u32 scratch[kCount];
    static u32 expected[kCount];

    auto fill = [](u32* target) noexcept {
        u64 state = 0x1234'5678'9ABC'DEF0ull;
        for (u64 i = 0; i < kCount; ++i) {
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            target[i] = static_cast<u32>((state * 0x2545'F491'4F6C'DD1Dull) >> 40);
        }
    };
    auto less = [](u32 a, u32 b) noexcept { return a < b; };

    fill(data);
    CY_REQUIRE(parallel_sort(system.get(), data, scratch, kCount, 64, less, "sort").has_value());
    for (u64 i = 1; i < kCount; ++i) {
        CY_REQUIRE(data[i - 1] <= data[i]);
    }
    for (u64 i = 0; i < kCount; ++i) {
        expected[i] = data[i];
    }

    fill(data);
    CY_REQUIRE(parallel_sort(system.get(), data, scratch, kCount, 64, less, "sort").has_value());
    for (u64 i = 0; i < kCount; ++i) {
        CY_REQUIRE_EQ(data[i], expected[i]);
    }
}

CY_TEST_CASE("chaos mode randomises permitted order without changing any result") {
    // The specification's scenario: chaos randomises permitted ordering so that a system relying on
    // execution order produces differing results and is identified. What must NOT change is
    // anything the dependency graph fixes — a dependency is still a dependency under chaos.
    JobSystemConfig config;
    config.worker_count = 4;
    config.mode = SchedulingMode::Chaos;
    config.chaos_seed = 0xDEAD'BEEF'CAFE'F00Dull;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());

    struct Chain {
        std::atomic<u32> stamp{0};
        std::atomic<u32> producer_at{0};
        std::atomic<u32> consumer_at{0};
        std::atomic<u32> unordered{0};
    };
    Chain chain;

    for (u32 round = 0; round < 32; ++round) {
        chain.stamp.store(0);
        auto producer = system->submit(
            [](const TaskContext&, void* user) noexcept {
                auto* state = static_cast<Chain*>(user);
                state->producer_at.store(state->stamp.fetch_add(1) + 1);
            },
            &chain, "producer", Priority::Background);
        CY_REQUIRE(producer.has_value());

        JobDesc consumer;
        consumer.name = "consumer";
        consumer.user = &chain;
        consumer.priority = Priority::Critical;
        consumer.body = [](const TaskContext&, void* user) noexcept {
            auto* state = static_cast<Chain*>(user);
            state->consumer_at.store(state->stamp.fetch_add(1) + 1);
        };
        const JobHandle dependencies[] = {producer.value()};
        consumer.dependencies = dependencies;
        consumer.dependency_count = 1;

        auto handle = system->submit(consumer);
        CY_REQUIRE(handle.has_value());
        system->wait(handle.value());

        // Critical before Background is what the priority classes would ask for; the dependency
        // says otherwise and wins, under chaos as under every other mode.
        if (chain.producer_at.load() >= chain.consumer_at.load()) {
            chain.unordered.fetch_add(1);
        }
    }
    CY_CHECK_EQ(chain.unordered.load(), 0u);
}

CY_TEST_CASE("deterministic mode ignores deadline hints") {
    // "In deterministic mode deadline hints SHALL be ignored and scheduling SHALL follow the fixed
    // order." Checked by running the same workload twice with opposite deadlines: the order does
    // not move.
    JobSystemConfig config;
    config.worker_count = 0;
    config.mode = SchedulingMode::DeterministicSingleThreaded;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());

    struct Item {
        Trace* trace;
        u32 index;
    };
    static Item items[32];
    Trace urgent_first;
    Trace urgent_last;

    for (u32 pass = 0; pass < 2; ++pass) {
        Trace& trace = pass == 0 ? urgent_first : urgent_last;
        JobHandle handles[32];
        for (u32 i = 0; i < 32; ++i) {
            items[i].trace = &trace;
            items[i].index = i;
            JobDesc desc;
            desc.name = "deadline";
            desc.user = &items[i];
            desc.deadline = Deadline{pass == 0 ? 32 - i : i + 1, 0};
            desc.body = [](const TaskContext&, void* user) noexcept {
                auto* item = static_cast<Item*>(user);
                item->trace->note(item->index);
            };
            auto handle = system->submit(desc);
            CY_REQUIRE(handle.has_value());
            handles[i] = handle.value();
        }
        system->wait_all(handles, 32);
    }

    for (u32 i = 0; i < 32; ++i) {
        CY_REQUIRE_EQ(urgent_first.order[i], i);
        CY_REQUIRE_EQ(urgent_last.order[i], i);
    }
}
