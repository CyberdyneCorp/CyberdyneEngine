// The job system under real workers. Tasks 3.2.1, 3.2.3, 3.2.6, 3.2.7, 3.2.8.
//
// Integration rather than unit, because every case here starts worker threads and the taxonomy
// gives a unit test one millisecond. What is being tested is the specification's scenarios:
// dependencies are honoured, a waiting worker does useful work, a parallel loop respects its grain,
// background work still progresses, scheduling does not allocate, a cancelled task does not run.

#include "harness.h"

#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/parallel.h>
#include <cy/core/jobs/sync.h>
#include <cy/core/jobs/thread_role.h>

#include <atomic>

namespace {

using namespace cy;
using namespace cy::jobs;
using cy::jobs::test::ScopedJobSystem;

struct Counter {
    std::atomic<u32> value{0};
};

void increment(const TaskContext&, void* user) noexcept {
    static_cast<Counter*>(user)->value.fetch_add(1, std::memory_order_relaxed);
}

/// A short spin. `Thread::sleep_for_ns` would hand the core back, which is the opposite of what a
/// test about occupancy wants.
void burn(u64 iterations) noexcept {
    volatile u64 sink = 0;
    for (u64 i = 0; i < iterations; ++i) {
        sink = sink + i;
    }
    (void)sink;
}

}  // namespace

CY_TEST_CASE("only one job system owns the worker threads") {
    ScopedJobSystem first(2);
    CY_REQUIRE(first.started());

    JobSystem second;
    JobSystemConfig config;
    config.worker_count = 2;
    const auto refused = second.start(config);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::AlreadyExists);

    CY_CHECK_EQ(JobSystem::current(), &first.get());
}

CY_TEST_CASE("the default worker count reserves the main thread") {
    ScopedJobSystem system(0);
    CY_REQUIRE(system.started());
    const u32 hardware = Thread::hardware_concurrency();
    CY_CHECK_EQ(system->worker_count(), hardware > 1 ? hardware - 1 : 1);
}

CY_TEST_CASE("a submitted job runs and its handle reports the outcome") {
    ScopedJobSystem system(3);
    CY_REQUIRE(system.started());

    Counter counter;
    auto handle = system->submit(&increment, &counter, "increment");
    CY_REQUIRE(handle.has_value());
    system->wait(handle.value());

    CY_CHECK_EQ(counter.value.load(), 1u);
    CY_CHECK(system->is_complete(handle.value()));
    const TaskOutcome outcome = system->outcome(handle.value());
    // Completed while the slot still holds this generation, Stale once it has been recycled. Both
    // mean "done", which is what `is_complete` reports and what a waiter needs.
    CY_CHECK((outcome == TaskOutcome::Completed || outcome == TaskOutcome::Stale));
}

CY_TEST_CASE("a job does not begin until its dependencies have completed") {
    ScopedJobSystem system(4);
    CY_REQUIRE(system.started());

    struct Chain {
        std::atomic<u32> order{0};
        std::atomic<u32> first_at{0};
        std::atomic<u32> second_at{0};
    };
    Chain chain;

    auto first = system->submit(
        [](const TaskContext&, void* user) noexcept {
            auto* state = static_cast<Chain*>(user);
            burn(50'000);
            state->first_at.store(state->order.fetch_add(1) + 1);
        },
        &chain, "first");
    CY_REQUIRE(first.has_value());

    JobDesc desc;
    desc.name = "second";
    desc.user = &chain;
    desc.body = [](const TaskContext&, void* user) noexcept {
        auto* state = static_cast<Chain*>(user);
        state->second_at.store(state->order.fetch_add(1) + 1);
    };
    const JobHandle dependencies[] = {first.value()};
    desc.dependencies = dependencies;
    desc.dependency_count = 1;

    auto second = system->submit(desc);
    CY_REQUIRE(second.has_value());
    system->wait(second.value());

    CY_CHECK_EQ(chain.first_at.load(), 1u);
    CY_CHECK_EQ(chain.second_at.load(), 2u);
}

CY_TEST_CASE("a null or stale dependency is already complete") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    Counter counter;
    JobDesc desc;
    desc.body = &increment;
    desc.user = &counter;
    desc.name = "unblocked";
    const JobHandle dependencies[] = {JobHandle{}, JobHandle::from_slot(0, 999999)};
    desc.dependencies = dependencies;
    desc.dependency_count = 2;

    auto handle = system->submit(desc);
    CY_REQUIRE(handle.has_value());
    system->wait(handle.value());
    CY_CHECK_EQ(counter.value.load(), 1u);
}

CY_TEST_CASE("a waiting worker runs other ready jobs rather than blocking") {
    // The specification's scenario, and the reason recursive submission cannot deadlock the pool:
    // one worker, and a job that submits another job and waits for it. A worker that blocked would
    // have nothing left to run the inner job.
    ScopedJobSystem system(1);
    CY_REQUIRE(system.started());

    Counter inner;
    auto outer = system->submit(
        [](const TaskContext& context, void* user) noexcept {
            auto* counter = static_cast<Counter*>(user);
            auto nested = context.system->submit(&increment, counter, "inner");
            if (nested) {
                context.system->wait(nested.value());
            }
            counter->value.fetch_add(10);
        },
        &inner, "outer");
    CY_REQUIRE(outer.has_value());
    system->wait(outer.value());
    CY_CHECK_EQ(inner.value.load(), 11u);
}

CY_TEST_CASE("a parallel loop partitions by grain and covers every index exactly once") {
    ScopedJobSystem system(4);
    CY_REQUIRE(system.started());

    constexpr u64 kCount = 10'000;
    struct Loop {
        std::atomic<u32> visits[kCount];
        std::atomic<u32> partitions{0};
    };
    static Loop loop;
    for (auto& visit : loop.visits) {
        visit.store(0);
    }

    auto handle = system->submit_parallel_for(
        kCount, 256,
        [](const TaskContext&, u64 begin, u64 end, void* user) noexcept {
            auto* state = static_cast<Loop*>(user);
            state->partitions.fetch_add(1);
            for (u64 i = begin; i < end; ++i) {
                state->visits[i].fetch_add(1);
            }
        },
        &loop, "visit");
    CY_REQUIRE(handle.has_value());
    system->wait(handle.value());

    for (auto& visit : loop.visits) {
        CY_REQUIRE_EQ(visit.load(), 1u);
    }
    // Ranges of at least the grain, so a small loop does not pay more in scheduling than it saves.
    CY_CHECK_EQ(loop.partitions.load(), JobSystem::partition_count(kCount, 256));
    CY_CHECK(loop.partitions.load() <= (kCount + 255) / 256);
}

CY_TEST_CASE("the partitioning is a pure function of the count and the grain") {
    // Not of the worker count: this is what makes a parallel loop reproducible, and it is checked
    // without a running system precisely because it cannot depend on one.
    CY_CHECK_EQ(JobSystem::partition_count(0, 16), 0u);
    CY_CHECK_EQ(JobSystem::partition_count(1, 16), 1u);
    CY_CHECK_EQ(JobSystem::partition_count(64, 16), 4u);
    CY_CHECK_EQ(JobSystem::partition_count(65, 16), 5u);
    // A grain of one over a million elements is capped rather than becoming a million tasks.
    CY_CHECK_EQ(JobSystem::partition_count(1'000'000, 1), kMaxParallelPartitions);

    u64 begin = 0;
    u64 end = 0;
    JobSystem::partition_range(65, 16, 0, begin, end);
    CY_CHECK_EQ(begin, 0u);
    CY_CHECK_EQ(end, 13u);
    JobSystem::partition_range(65, 16, 4, begin, end);
    CY_CHECK_EQ(begin, 52u);
    CY_CHECK_EQ(end, 65u);
    // Out of range is an empty range, never an out-of-bounds one.
    JobSystem::partition_range(65, 16, 99, begin, end);
    CY_CHECK_EQ(begin, end);
}

CY_TEST_CASE("an empty parallel loop is a null handle, which reads as complete") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());
    auto handle = system->submit_parallel_for(
        0, 16, [](const TaskContext&, u64, u64, void*) noexcept {}, nullptr, "empty");
    CY_REQUIRE(handle.has_value());
    CY_CHECK(handle.value().is_null());
    CY_CHECK(system->is_complete(handle.value()));
}

CY_TEST_CASE("scheduling ten thousand tasks touches the general allocator not at all") {
    // The specification's scenario. Task records come from per-participant slabs and a task's
    // arguments travel inside the record, so a frame that schedules ten thousand tasks makes no
    // general-heap allocation at all — which the system counts rather than the test assuming.
    JobSystemConfig config;
    config.worker_count = 4;
    // Sized for the whole burst: `task_slots_per_participant` is the documented bound on how many
    // tasks one thread may have in flight, and this thread submits ten thousand without pausing.
    config.task_slots_per_participant = 16384;
    config.deque_capacity = 16384;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());
    system->reset_stats();

    Counter counter;
    constexpr u32 kTasks = 10'000;
    for (u32 i = 0; i < kTasks; ++i) {
        auto handle = system->submit(&increment, &counter, "tiny");
        CY_REQUIRE(handle.has_value());
    }
    system->wait_for_idle();

    CY_CHECK_EQ(counter.value.load(), kTasks);
    const JobSystemStats stats = system->stats();
    CY_CHECK_EQ(stats.tasks_submitted, kTasks);
    CY_CHECK_EQ(stats.tasks_executed, kTasks);
    CY_CHECK_EQ(stats.scheduling_allocations, 0u);
    CY_CHECK_EQ(stats.slab_exhaustions, 0u);
}

CY_TEST_CASE("a task context carries its worker, its scratch and its name") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    struct Seen {
        std::atomic<bool> had_scratch{false};
        std::atomic<bool> had_index{false};
        std::atomic<bool> named{false};
        std::atomic<bool> scratch_worked{false};
        std::atomic<bool> knew_itself{false};
    };
    Seen seen;

    auto handle = system->submit(
        [](const TaskContext& context, void* user) noexcept {
            auto* state = static_cast<Seen*>(user);
            state->had_scratch.store(context.scratch != nullptr);
            // A participant index, not necessarily one of the system's own workers: the thread that
            // waits also runs ready tasks, and it holds a helper slot while it does.
            state->had_index.store(context.worker != kNotAWorker);
            state->named.store(context.name != nullptr && context.name[0] == 'n');
            state->knew_itself.store(!context.self.is_null() && context.system != nullptr);
            u32* temporary = context.allocate<u32>(64);
            if (temporary != nullptr) {
                temporary[0] = 1;
                temporary[63] = 2;
                state->scratch_worked.store(temporary[0] + temporary[63] == 3);
            }
        },
        &seen, "named");
    CY_REQUIRE(handle.has_value());
    system->wait(handle.value());

    CY_CHECK(seen.had_scratch.load());
    CY_CHECK(seen.had_index.load());
    CY_CHECK(seen.named.load());
    CY_CHECK(seen.scratch_worked.load());
    CY_CHECK(seen.knew_itself.load());
}

CY_TEST_CASE("a task cancelled before it begins does not run its body") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    auto source = CancellationSource::create();
    CY_REQUIRE(source.has_value());
    source.value().cancel();

    Counter counter;
    JobDesc desc;
    desc.body = &increment;
    desc.user = &counter;
    desc.name = "cancelled";
    desc.cancellation = source.value().token();

    auto handle = system->submit(desc);
    CY_REQUIRE(handle.has_value());
    system->wait(handle.value());

    CY_CHECK_EQ(counter.value.load(), 0u);
    CY_CHECK(system->stats().tasks_cancelled >= 1);
}

CY_TEST_CASE("long-running work observes its token at a bounded interval") {
    // Cooperative: nothing is forcibly terminated. The task checks its token between chunks,
    // releases what it holds, and returns — which is exactly what chunked long-running work does.
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    auto source = CancellationSource::create();
    CY_REQUIRE(source.has_value());

    struct Chunked {
        std::atomic<u32> chunks{0};
        std::atomic<bool> observed{false};
        Event started;
    };
    Chunked chunked;

    JobDesc desc;
    desc.name = "chunked";
    desc.user = &chunked;
    desc.cancellation = source.value().token();
    desc.priority = Priority::Background;
    desc.body = [](const TaskContext& context, void* user) noexcept {
        auto* state = static_cast<Chunked*>(user);
        state->started.set();
        for (u32 chunk = 0; chunk < 100'000; ++chunk) {
            if (context.is_cancelled()) {
                state->observed.store(true);
                return;
            }
            state->chunks.fetch_add(1);
            burn(200);
        }
    };

    auto handle = system->submit(desc);
    CY_REQUIRE(handle.has_value());
    chunked.started.wait();
    source.value().cancel();
    system->wait(handle.value());

    CY_CHECK(chunked.observed.load());
    CY_CHECK(chunked.chunks.load() < 100'000u);
}

CY_TEST_CASE("background work still progresses under sustained high-priority load") {
    // The specification's fairness scenario, as a number. High-priority work saturates the pool;
    // the fairness quantum is what guarantees the background tasks bounded minimum progress rather
    // than being served only once the queue drains.
    JobSystemConfig config;
    config.worker_count = 2;
    config.fairness_quantum = 8;
    config.task_slots_per_participant = 4096;
    config.deque_capacity = 4096;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());

    struct Fairness {
        std::atomic<u32> completed{0};
        std::atomic<u32> last_background{0};
    };
    Fairness fairness;

    constexpr u32 kHigh = 2000;
    constexpr u32 kBackground = 10;

    // Background first, high afterwards. The order matters to what this measures: submitted the
    // other way round, the background tasks arrive after the pool has already chewed through half
    // the high-priority queue, and the test would pass without the fairness sweep existing at all.
    for (u32 i = 0; i < kBackground; ++i) {
        JobDesc desc;
        desc.name = "background";
        desc.priority = Priority::Background;
        desc.user = &fairness;
        desc.body = [](const TaskContext&, void* user) noexcept {
            auto* state = static_cast<Fairness*>(user);
            state->last_background.store(state->completed.fetch_add(1) + 1);
        };
        CY_REQUIRE(system->submit(desc).has_value());
    }
    for (u32 i = 0; i < kHigh; ++i) {
        JobDesc desc;
        desc.name = "high";
        desc.priority = Priority::High;
        desc.user = &fairness;
        desc.body = [](const TaskContext&, void* user) noexcept {
            burn(400);
            static_cast<Fairness*>(user)->completed.fetch_add(1);
        };
        CY_REQUIRE(system->submit(desc).has_value());
    }

    system->wait_for_idle();
    CY_CHECK_EQ(fairness.completed.load(), kHigh + kBackground);
    // Bounded minimum progress, as a number: with a quantum of eight and ten background tasks, the
    // last of them is served within about eighty pops. Four hundred is that with room for the
    // scheduling noise of a loaded machine — and it is still a twentieth of the queue, so a
    // scheduler that served them only when the high-priority work ran out would fail here.
    CY_CHECK(fairness.last_background.load() < 400u);
}

CY_TEST_CASE("statistics report throughput, stealing and occupancy") {
    ScopedJobSystem system(4);
    CY_REQUIRE(system.started());
    system->reset_stats();

    struct Work {
        std::atomic<u32> done{0};
        Event finished;
    };
    Work work;

    JobHandle handles[64];
    for (u32 i = 0; i < 64; ++i) {
        JobDesc desc;
        desc.name = "work";
        desc.user = &work;
        desc.priority = i % 2 == 0 ? Priority::High : Priority::Normal;
        desc.body = [](const TaskContext&, void* user) noexcept {
            auto* state = static_cast<Work*>(user);
            burn(20'000);
            if (state->done.fetch_add(1) + 1 == 64) {
                state->finished.set();
            }
        };
        auto handle = system->submit(desc);
        CY_REQUIRE(handle.has_value());
        handles[i] = handle.value();
    }

    // Block rather than wait. `wait()` runs ready tasks on this thread, and a thread that drains
    // its own submissions steals from nobody — which is exactly what made the assertion below
    // fail in the Shipping configuration, where the bodies are fast enough for one thread to get
    // through all sixty-four before a worker looks.
    work.finished.wait();
    system->wait_all(handles, 64);

    const JobSystemStats stats = system->stats();
    CY_CHECK_EQ(stats.tasks_executed, 64u);
    CY_CHECK_EQ(stats.executed_by_priority[static_cast<u32>(Priority::High)], 32u);
    CY_CHECK_EQ(stats.executed_by_priority[static_cast<u32>(Priority::Normal)], 32u);
    CY_CHECK(stats.worker_busy_ns > 0);
    CY_CHECK(stats.queue_latency_samples > 0);
    CY_CHECK_EQ(stats.worker_count, 4u);
    // Everything was submitted from one thread, so a worker can only have got it by stealing. This
    // is the work-stealing requirement as an observation rather than an assumption.
    CY_CHECK(stats.steal_successes > 0);
}

CY_TEST_CASE("a job with no body is refused rather than scheduled") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    JobDesc desc;
    desc.name = "bodyless";
    const auto refused = system->submit(desc);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("inline arguments larger than the record are refused, not truncated") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    u8 payload[kMaxInlineArgumentBytes + 1] = {};
    JobDesc desc;
    desc.body = [](const TaskContext&, void*) noexcept {};
    desc.name = "oversized";
    desc.inline_data = payload;
    desc.inline_size = sizeof(payload);

    const auto refused = system->submit(desc);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}
