// Workers never block on I/O or the GPU. Task 3.2.5 — "a test that tries to, and fails".
//
// This file is that test. A worker attempts to block; the attempt is refused, the violation is
// counted in every configuration, and the alternative — hand the operation to the dedicated async
// thread and let its completion schedule a continuation — is exercised beside it so that the
// refusal is not merely a prohibition.

#include "harness.h"

#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/thread_role.h>

#include <atomic>
#include <cstdio>

namespace {

using namespace cy;
using namespace cy::jobs;
using cy::jobs::test::ScopedJobSystem;

struct BlockingAttempt {
    std::atomic<bool> permitted{true};
    std::atomic<u32> error_code{0};
};

}  // namespace

CY_TEST_CASE("a thread executing a job that tries to block on I/O is refused and counted") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());
    reset_blocking_violations();

    BlockingAttempt attempt;
    auto handle = system->submit(
        [](const TaskContext& context, void* user) noexcept {
            auto* state = static_cast<BlockingAttempt*>(user);
            // The call a task must not make. It is refused before anything blocks: the point is
            // that the worker is not removed from the pool, not that it is warned afterwards.
            const Status permitted =
                context.system->begin_blocking_region("read the package index");
            state->permitted.store(permitted.has_value());
            if (!permitted) {
                state->error_code.store(static_cast<u32>(permitted.error().code));
            } else {
                context.system->end_blocking_region();
            }
        },
        &attempt, "would-block");
    CY_REQUIRE(handle.has_value());
    system->wait(handle.value());

    CY_CHECK_FALSE(attempt.permitted.load());
    CY_CHECK_EQ(attempt.error_code.load(), static_cast<u32>(ErrorCode::Unsupported));
    // Which thread ran the body is deliberately not asserted: it may be a worker, or it may be the
    // thread that called `wait()`, because a waiting thread runs ready tasks. The refusal does not
    // depend on which, and neither does this test.
    // Compiled into every configuration. A suite that asserted only on the assertion would report
    // nothing in Profile and Shipping — the exact mistake that made M0's suite red in two profiles.
    CY_CHECK_EQ(blocking_violations(), 1u);
    CY_CHECK(last_blocking_violation()[0] != '\0');
    CY_CHECK_EQ(system->stats().blocking_violations, 1u);
    reset_blocking_violations();
}

CY_TEST_CASE("a blocking region is permitted off the worker threads") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());
    reset_blocking_violations();
    clear_thread_roles();
    set_thread_role(ThreadRole::Main);

    // The main thread may block: it is not part of the pool, so blocking it costs the frame rather
    // than a worker. The region is still declared, which is what lets the watchdog report one held
    // too long.
    CY_CHECK(system->begin_blocking_region("main thread work").has_value());
    system->end_blocking_region();
    CY_CHECK_EQ(blocking_violations(), 0u);

    clear_thread_roles();
}

CY_TEST_CASE("a file read is submitted asynchronously and its continuation is scheduled") {
    // The specification's scenario: a task reads a file, the read is submitted asynchronously, the
    // worker runs other ready work, and the continuation is scheduled on completion.
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());
    reset_blocking_violations();

    AsyncService async;
    CY_REQUIRE(async.start(system.get()).has_value());

    struct Read {
        std::atomic<u64> bytes{0};
        std::atomic<bool> on_io_thread{false};
        std::atomic<bool> continuation_ran{false};
    };
    static Read read;
    read.bytes.store(0);
    read.on_io_thread.store(false);
    read.continuation_ran.store(false);

    // A real file, written here rather than assumed: __FILE__ is made relative by
    // -fmacro-prefix-map (so that no artefact carries the build machine's directory layout) and a
    // test runs from the build tree, so the source is not where the compiler's spelling of it says.
    {
        std::FILE* seed = std::fopen("jobs_async_read.tmp", "wb");
        CY_REQUIRE(seed != nullptr);
        for (u32 i = 0; i < 512; ++i) {
            std::fputc('x', seed);
        }
        std::fclose(seed);
    }

    // The blocking call itself, on the thread where blocking is legal.
    auto operation = [](void* user) noexcept {
        auto* state = static_cast<Read*>(user);
        state->on_io_thread.store(thread_holds_role(ThreadRole::AssetIo));
        std::FILE* file = std::fopen("jobs_async_read.tmp", "rb");
        if (file == nullptr) {
            return;
        }
        char buffer[4096];
        const usize count = std::fread(buffer, 1, sizeof(buffer), file);
        std::fclose(file);
        state->bytes.store(count);
    };

    auto io = async.submit_blocking(operation, &read, "read.source");
    CY_REQUIRE(io.has_value());

    // The continuation: an ordinary job that depends on the completion. It is scheduled when the
    // read finishes, and it may run on a different worker from the one that submitted it.
    JobDesc continuation;
    continuation.name = "read.continuation";
    continuation.user = &read;
    continuation.body = [](const TaskContext&, void* user) noexcept {
        static_cast<Read*>(user)->continuation_ran.store(true);
    };
    const JobHandle dependencies[] = {io.value()};
    continuation.dependencies = dependencies;
    continuation.dependency_count = 1;

    auto scheduled = system->submit(continuation);
    CY_REQUIRE(scheduled.has_value());
    system->wait(scheduled.value());

    CY_CHECK(read.bytes.load() > 0);
    CY_CHECK(read.on_io_thread.load());
    CY_CHECK(read.continuation_ran.load());
    CY_CHECK_EQ(async.operations_completed(), 1u);
    // No worker was ever inside a blocking region, which is the property the whole arrangement
    // exists to preserve.
    CY_CHECK_EQ(blocking_violations(), 0u);

    async.stop();
    std::remove("jobs_async_read.tmp");
}

CY_TEST_CASE("a GPU fence is awaited through a completion notification, never by spinning") {
    // The specification's readback scenario. The fence is a gated job; a backend's completion
    // notification releases it. Nothing here waits on the GPU: the dependent job is not runnable
    // until the signal arrives, so no worker is occupied in the meantime.
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    FenceSignal fence;
    CY_REQUIRE(fence.arm(system.get(), "readback.fence").has_value());
    CY_CHECK_FALSE(fence.is_signalled());

    std::atomic<bool> consumed{false};
    JobDesc consumer;
    consumer.name = "readback.consume";
    consumer.user = &consumed;
    consumer.body = [](const TaskContext&, void* user) noexcept {
        static_cast<std::atomic<bool>*>(user)->store(true);
    };
    const JobHandle dependencies[] = {fence.handle()};
    consumer.dependencies = dependencies;
    consumer.dependency_count = 1;

    auto handle = system->submit(consumer);
    CY_REQUIRE(handle.has_value());

    // Still gated: the consumer cannot have run.
    CY_CHECK_FALSE(consumed.load());
    CY_CHECK_FALSE(system->is_complete(handle.value()));

    fence.signal();
    system->wait(handle.value());
    CY_CHECK(consumed.load());
    CY_CHECK(fence.is_signalled());

    // Idempotent, because a driver may notify more than once.
    fence.signal();
}

CY_TEST_CASE("a timer completes a handle without occupying a worker") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    AsyncService async;
    CY_REQUIRE(async.start(system.get()).has_value());

    const i64 started = monotonic_now_ns();
    auto timer = async.after(5'000'000, "timer.5ms");
    CY_REQUIRE(timer.has_value());
    system->wait(timer.value());
    const i64 elapsed = monotonic_now_ns() - started;

    CY_CHECK(elapsed >= 4'000'000);
    CY_CHECK_EQ(async.timers_fired(), 1u);
    async.stop();
}

CY_TEST_CASE("a worker held inside a declared blocking region is reported by name") {
    // The specification's "blocking is detected" scenario. The region is declared on a thread where
    // blocking is legal — a worker's attempt would have been refused outright — and held past the
    // threshold, so the watchdog reports it.
    JobSystemConfig config;
    config.worker_count = 1;
    config.blocked_worker_threshold_ns = 5'000'000;  // 5 ms
    config.watchdog_interval_ns = 1'000'000;         // 1 ms
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());

    clear_thread_roles();
    set_thread_role(ThreadRole::AssetIo);
    CY_REQUIRE(system->begin_blocking_region("a deliberately long read").has_value());
    Thread::sleep_for_ns(40'000'000);
    system->end_blocking_region();

    CY_CHECK(system->stats().blocked_worker_detections >= 1);
    clear_thread_roles();
}

CY_TEST_CASE("a task that runs longer than the limit is reported with its name") {
    // Task 3.2.8: long-running work is chunked or yields, and a development build reports a task
    // that exceeds a configurable duration. The report is a counter, so it is live in all four
    // configurations rather than only where assertions are.
    JobSystemConfig config;
    config.worker_count = 2;
    config.long_task_threshold_ns = 5'000'000;  // 5 ms
    config.watchdog_interval_ns = 1'000'000;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());

    auto handle =
        system->submit([](const TaskContext&, void*) noexcept { Thread::sleep_for_ns(40'000'000); },
                       nullptr, "deliberately-overlong");
    CY_REQUIRE(handle.has_value());
    system->wait(handle.value());

    CY_CHECK(system->stats().long_task_detections >= 1);
}

CY_TEST_CASE("a cancelled task that never observes its token is reported") {
    // The specification's "unresponsive task is reported" scenario. Cancellation is cooperative:
    // nothing terminates the task, and the only remedy is to say so.
    JobSystemConfig config;
    config.worker_count = 2;
    config.cancellation_grace_ns = 5'000'000;
    config.long_task_threshold_ns = 1'000'000'000;  // out of the way of this measurement
    config.watchdog_interval_ns = 1'000'000;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());

    auto source = CancellationSource::create();
    CY_REQUIRE(source.has_value());

    Event running;
    JobDesc desc;
    desc.name = "ignores-cancellation";
    desc.cancellation = source.value().token();
    desc.user = &running;
    desc.body = [](const TaskContext&, void* user) noexcept {
        static_cast<Event*>(user)->set();
        Thread::sleep_for_ns(60'000'000);
    };

    auto handle = system->submit(desc);
    CY_REQUIRE(handle.has_value());
    running.wait();
    source.value().cancel();
    system->wait(handle.value());

    CY_CHECK(system->stats().unresponsive_cancellations >= 1);
}
