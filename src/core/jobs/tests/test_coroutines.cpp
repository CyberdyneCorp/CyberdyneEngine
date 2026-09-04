// Coroutines as the asynchronous model. Task 3.2.4.
//
// The scenarios: a load reads as a sequence rather than as a chain of callbacks; a continuation is
// scheduled as a task and may run on a different worker; awaiting does not allocate from the
// general heap on the common path.

#include "harness.h"

#include <cy/core/jobs/async.h>
#include <cy/core/jobs/coroutine.h>
#include <cy/core/jobs/job_system.h>

#include <atomic>
#include <cstdio>

namespace {

using namespace cy;
using namespace cy::jobs;
using cy::jobs::test::ScopedJobSystem;

struct LoadState {
    std::atomic<u32> steps{0};
    std::atomic<u64> bytes{0};
    std::atomic<u32> worker_at_start{kNotAWorker};
    std::atomic<u32> worker_at_end{kNotAWorker};
};

constexpr const char* kCellFile = "jobs_cell_load.tmp";

/// Write the file the load reads. __FILE__ is made relative by -fmacro-prefix-map — so that no
/// artefact carries the build machine's directory layout — and a test runs from the build tree, so
/// the source is not where the compiler's spelling of it says.
void write_cell_file() noexcept {
    std::FILE* file = std::fopen(kCellFile, "wb");
    if (file == nullptr) {
        return;
    }
    for (u32 i = 0; i < 512; ++i) {
        std::fputc('c', file);
    }
    std::fclose(file);
}

void read_cell_file(void* user) noexcept {
    auto* state = static_cast<LoadState*>(user);
    std::FILE* file = std::fopen(kCellFile, "rb");
    if (file == nullptr) {
        return;
    }
    char buffer[2048];
    state->bytes.store(std::fread(buffer, 1, sizeof(buffer), file));
    std::fclose(file);
}

Task<int> add_one(int value) {
    co_return value + 1;
}

Task<int> add_three(int value) {
    const int first = co_await add_one(value);
    const int second = co_await add_one(first);
    co_return co_await add_one(second);
}

}  // namespace

CY_TEST_CASE("a coroutine composes with another and yields its value") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    Task<int> task = add_three(39);
    CY_REQUIRE(task.is_valid());
    CY_REQUIRE(sync_await(system.get(), task).has_value());
    CY_CHECK(task.done());
    CY_CHECK_EQ(task.result(), 42);
}

CY_TEST_CASE("a void coroutine completes and signals its handle") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    static std::atomic<u32> ran{0};
    ran.store(0);

    auto make = []() -> Task<void> {
        ran.fetch_add(1);
        co_return;
    };
    Task<void> task = make();
    CY_REQUIRE(task.is_valid());

    auto handle = co_spawn(system.get(), task, "void.coroutine");
    CY_REQUIRE(handle.has_value());
    system->wait(handle.value());
    CY_CHECK_EQ(ran.load(), 1u);
    CY_CHECK(task.done());
}

CY_TEST_CASE("a cell load reads as a sequence, and its worker is released while it waits") {
    // The specification's scenario: "a cell load reads a file, decompresses it, and activates the
    // result THEN it SHALL be written as a coroutine awaiting each step, not as a chain of
    // callbacks". Each `co_await` here releases the worker; the continuation is an ordinary task.
    ScopedJobSystem system(3);
    CY_REQUIRE(system.started());
    reset_blocking_violations();

    AsyncService async;
    CY_REQUIRE(async.start(system.get()).has_value());

    static LoadState state;
    state.steps.store(0);
    state.bytes.store(0);
    state.worker_at_start.store(kNotAWorker);
    state.worker_at_end.store(kNotAWorker);
    write_cell_file();

    JobSystem& jobs = system.get();
    AsyncService& service = async;

    auto load_cell = [&jobs, &service]() -> Task<void> {
        state.steps.fetch_add(1);

        // Step one: the read. Submitted to the thread where blocking is legal; this coroutine
        // suspends, and the worker that was running it goes back to its deque.
        auto io = service.submit_blocking(&read_cell_file, &state, "cell.read");
        if (io) {
            co_await await_job(jobs, io.value());
        }
        state.steps.fetch_add(1);

        // Step two: the decompression, as an ordinary job. Awaiting it is the same mechanism.
        auto decompress =
            jobs.submit([](const TaskContext& context,
                           void*) noexcept { state.worker_at_start.store(context.worker); },
                        nullptr, "cell.decompress");
        if (decompress) {
            co_await await_job(jobs, decompress.value());
        }
        state.steps.fetch_add(1);

        // Step three: activation, on whichever worker resumed the continuation.
        state.worker_at_end.store(current_worker_index());
        co_return;
    };

    Task<void> cell = load_cell();
    CY_REQUIRE(cell.is_valid());
    CY_REQUIRE(sync_await(jobs, cell, "cell.load").has_value());

    CY_CHECK_EQ(state.steps.load(), 3u);
    CY_CHECK(state.bytes.load() > 0);
    // No worker ever entered a blocking region: the read happened on the async thread and the
    // coroutine was suspended, not parked.
    CY_CHECK_EQ(blocking_violations(), 0u);

    async.stop();
    std::remove(kCellFile);
}

CY_TEST_CASE("awaiting an already-complete job does not suspend") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    auto done = system->submit([](const TaskContext&, void*) noexcept {}, nullptr, "done");
    CY_REQUIRE(done.has_value());
    system->wait(done.value());

    JobSystem& jobs = system.get();
    const JobHandle completed = done.value();
    static std::atomic<bool> resumed{false};
    resumed.store(false);

    auto body = [&jobs, completed]() -> Task<void> {
        co_await await_job(jobs, completed);
        resumed.store(true);
        co_return;
    };
    Task<void> task = body();
    CY_REQUIRE(task.is_valid());
    CY_REQUIRE(sync_await(jobs, task).has_value());
    CY_CHECK(resumed.load());
}

CY_TEST_CASE("a coroutine awaits a cancellation") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    auto source = CancellationSource::create();
    CY_REQUIRE(source.has_value());

    JobSystem& jobs = system.get();
    CancellationToken token = source.value().token();
    static std::atomic<bool> observed{false};
    observed.store(false);

    auto body = [&jobs, token]() -> Task<void> {
        co_await await_cancellation(jobs, token);
        observed.store(true);
        co_return;
    };
    Task<void> task = body();
    CY_REQUIRE(task.is_valid());

    auto handle = co_spawn(jobs, task, "await.cancellation");
    CY_REQUIRE(handle.has_value());
    CY_CHECK_FALSE(observed.load());

    source.value().cancel();
    jobs.wait(handle.value());
    CY_CHECK(observed.load());
}

CY_TEST_CASE("a coroutine awaiting an already-cancelled token does not suspend") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    auto source = CancellationSource::create();
    CY_REQUIRE(source.has_value());
    source.value().cancel();

    JobSystem& jobs = system.get();
    CancellationToken token = source.value().token();
    static std::atomic<bool> observed{false};
    observed.store(false);

    auto body = [&jobs, token]() -> Task<void> {
        co_await await_cancellation(jobs, token);
        observed.store(true);
        co_return;
    };
    Task<void> task = body();
    CY_REQUIRE(sync_await(jobs, task).has_value());
    CY_CHECK(observed.load());
}

CY_TEST_CASE("coroutine frames come from the per-thread slabs, not the general heap") {
    // "Awaiting SHALL never allocate from the general heap on the common path; coroutine frames
    // SHALL come from per-worker slabs where their size is known." The slab is warm after the first
    // few frames, so the interesting number is that nothing fell back to the heap.
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    const CoroutineFrameStats before = coroutine_frame_stats();
    for (u32 i = 0; i < 200; ++i) {
        Task<int> task = add_one(static_cast<int>(i));
        CY_REQUIRE(task.is_valid());
        CY_REQUIRE(sync_await(system.get(), task).has_value());
        CY_REQUIRE_EQ(task.result(), static_cast<int>(i) + 1);
    }
    const CoroutineFrameStats after = coroutine_frame_stats();

    CY_CHECK(after.from_slab - before.from_slab >= 200);
    CY_CHECK_EQ(after.from_heap, before.from_heap);
    // Every frame allocated here was destroyed here.
    CY_CHECK_EQ(after.live, before.live);
}

CY_TEST_CASE("a task group is a job handle, so awaiting one is the same mechanism") {
    ScopedJobSystem system(3);
    CY_REQUIRE(system.started());

    static std::atomic<u32> visited{0};
    visited.store(0);

    auto group = system->submit_parallel_for(
        1000, 64,
        [](const TaskContext&, u64 begin, u64 end, void*) noexcept {
            visited.fetch_add(static_cast<u32>(end - begin));
        },
        nullptr, "group");
    CY_REQUIRE(group.has_value());

    JobSystem& jobs = system.get();
    const JobHandle handle = group.value();
    static std::atomic<bool> after_group{false};
    after_group.store(false);

    auto body = [&jobs, handle]() -> Task<void> {
        co_await await_job(jobs, handle);
        after_group.store(visited.load() == 1000);
        co_return;
    };
    Task<void> task = body();
    CY_REQUIRE(sync_await(jobs, task).has_value());
    CY_CHECK(after_group.load());
}
