// Concurrency diagnostics, and the critical path. Task 3.2.11.
//
// The specification's ordering requirement is what makes this a suite rather than a nicety: "task
// profiling and critical-path reporting SHALL exist before work-stealing behaviour is tuned, since
// throughput improvements off the critical path do not shorten frames". So the critical path is
// tested against a graph whose longest chain is known by construction, and the job events are read
// back off the M0 shared trace rather than out of a second mechanism.

#include "harness.h"

#include <cy/core/diagnostics/trace.h>
#include <cy/core/jobs/diagnostics.h>
#include <cy/core/jobs/job_system.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace {

using namespace cy;
using namespace cy::jobs;
using cy::jobs::test::ScopedJobSystem;

void burn_ms(i64 milliseconds) noexcept {
    const i64 until = monotonic_now_ns() + milliseconds * 1'000'000;
    volatile u64 sink = 0;
    while (monotonic_now_ns() < until) {
        for (u32 i = 0; i < 1000; ++i) {
            sink = sink + i;
        }
    }
    (void)sink;
}

void long_step(const TaskContext&, void*) noexcept {
    burn_ms(4);
}

void short_step(const TaskContext&, void*) noexcept {
    burn_ms(1);
}

/// A temporary path for a trace artefact, removed by the destructor.
class ScopedTraceFile {
public:
    explicit ScopedTraceFile(const char* name) : path_(name) {}
    ~ScopedTraceFile() { std::remove(path_.c_str()); }

    ScopedTraceFile(const ScopedTraceFile&) = delete;
    ScopedTraceFile& operator=(const ScopedTraceFile&) = delete;

    [[nodiscard]] const char* path() const noexcept { return path_.c_str(); }

private:
    std::string path_;
};

}  // namespace

CY_TEST_CASE("the critical path is the longest chain, not the sum of the durations") {
    ScopedJobSystem system(4);
    CY_REQUIRE(system.started());
    system->begin_frame(1);

    // A chain of three long steps, and six short steps beside it with no dependencies. The sum of
    // every task's duration is far larger than the chain; the frame's duration is the chain.
    auto first = system->submit(&long_step, nullptr, "chain.first");
    CY_REQUIRE(first.has_value());

    JobDesc second_desc;
    second_desc.body = &long_step;
    second_desc.name = "chain.second";
    const JobHandle first_dependency[] = {first.value()};
    second_desc.dependencies = first_dependency;
    second_desc.dependency_count = 1;
    auto second = system->submit(second_desc);
    CY_REQUIRE(second.has_value());

    JobDesc third_desc;
    third_desc.body = &long_step;
    third_desc.name = "chain.third";
    const JobHandle second_dependency[] = {second.value()};
    third_desc.dependencies = second_dependency;
    third_desc.dependency_count = 1;
    auto third = system->submit(third_desc);
    CY_REQUIRE(third.has_value());

    JobHandle beside[6];
    for (u32 i = 0; i < 6; ++i) {
        auto handle = system->submit(&short_step, nullptr, "beside");
        CY_REQUIRE(handle.has_value());
        beside[i] = handle.value();
    }

    system->wait(third.value());
    system->wait_all(beside, 6);

    const CriticalPath path = system->critical_path();
    CY_CHECK_EQ(path.frame_index, 1u);
    CY_CHECK_EQ(path.entries_dropped, 0u);
    CY_CHECK_EQ(path.tasks_recorded, 9u);
    CY_REQUIRE_EQ(path.length, 3u);

    // Oldest first, which is the order a timeline is read in.
    CY_CHECK(std::string(path.entries[0].name) == "chain.first");
    CY_CHECK(std::string(path.entries[1].name) == "chain.second");
    CY_CHECK(std::string(path.entries[2].name) == "chain.third");

    // Each entry's chain includes everything before it.
    CY_CHECK(path.entries[1].path_ns > path.entries[0].path_ns);
    CY_CHECK(path.entries[2].path_ns > path.entries[1].path_ns);
    CY_CHECK_EQ(path.total_ns, path.entries[2].path_ns);

    // The point of the whole report: the sum of the durations is much larger than the chain, so
    // optimising the six tasks beside it would not shorten the frame by a nanosecond.
    CY_CHECK(path.total_task_ns > path.total_ns);
}

CY_TEST_CASE("a new frame resets the critical path") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());

    system->begin_frame(10);
    auto first = system->submit(&short_step, nullptr, "frame.10");
    CY_REQUIRE(first.has_value());
    system->wait(first.value());
    CY_CHECK_EQ(system->critical_path().tasks_recorded, 1u);

    system->begin_frame(11);
    const CriticalPath fresh = system->critical_path();
    CY_CHECK_EQ(fresh.frame_index, 11u);
    CY_CHECK_EQ(fresh.tasks_recorded, 0u);
    CY_CHECK_EQ(fresh.length, 0u);
}

CY_TEST_CASE("a frame log that overflows says so rather than reporting a shorter path") {
    JobSystemConfig config;
    config.worker_count = 2;
    config.frame_log_entries = 16;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());
    system->begin_frame(1);

    for (u32 i = 0; i < 64; ++i) {
        auto handle = system->submit([](const TaskContext&, void*) noexcept {}, nullptr, "tiny");
        CY_REQUIRE(handle.has_value());
    }
    system->wait_for_idle();

    const CriticalPath path = system->critical_path();
    CY_CHECK_EQ(path.tasks_recorded, 16u);
    CY_CHECK(path.entries_dropped > 0);
}

CY_TEST_CASE("idle and busy time are both reported, so an idle worker can be explained") {
    JobSystemConfig config;
    config.worker_count = 3;
    ScopedJobSystem system(config);
    CY_REQUIRE(system.started());
    system->reset_stats();

    // A strictly serial chain on a three-worker pool: two workers can have nothing to do, and the
    // report is what says whether that is dependency serialisation or insufficient partitioning.
    JobHandle previous;
    for (u32 i = 0; i < 4; ++i) {
        JobDesc desc;
        desc.body = &short_step;
        desc.name = "serial";
        JobHandle dependency[1];
        if (!previous.is_null()) {
            dependency[0] = previous;
            desc.dependencies = dependency;
            desc.dependency_count = 1;
        }
        auto handle = system->submit(desc);
        CY_REQUIRE(handle.has_value());
        previous = handle.value();
    }
    system->wait(previous);

    const JobSystemStats stats = system->stats();
    CY_CHECK_EQ(stats.tasks_executed, 4u);
    CY_CHECK(stats.worker_busy_ns > 0);
    CY_CHECK(stats.worker_idle_ns > 0);
}

CY_TEST_CASE("job and task events land on the M0 shared trace") {
    // "Job and task events SHALL be emitted into the shared trace ... so that task behaviour
    // correlates with memory, streaming, GPU, and simulation events on one timeline rather than in
    // a separate tool." Verified by opening the one trace and reading its own statistics back.
    const ScopedTraceFile artefact("jobs_trace_test.cytrace");

    diag::TraceConfig trace;
    trace.path = artefact.path();
    trace.consumer_thread = false;
    trace.build_identity = "jobs-suite";
    CY_REQUIRE(diag::trace_open(trace).has_value());

    JobSystemConfig config;
    config.worker_count = 2;
    config.emit_trace = true;
    {
        ScopedJobSystem system(config);
        CY_REQUIRE(system.started());
        system->begin_frame(1);

        for (u32 i = 0; i < 32; ++i) {
            auto handle = system->submit([](const TaskContext&, void*) noexcept {}, nullptr,
                                         "traced");
            CY_REQUIRE(handle.has_value());
        }
        system->wait_for_idle();

        jobs_trace_report(system->stats());
        jobs_trace_critical_path(system->critical_path());
        jobs_log_report(system->stats());
    }

    diag::trace_flush();
    const auto closed = diag::trace_close();
    CY_REQUIRE(closed.has_value());

    // Two records per task, plus the counters, the snapshot and the critical-path records.
    CY_CHECK(closed.value().events_emitted >= 64);
    CY_CHECK(closed.value().events_written > 0);
    // A field with no classification would have been redacted and counted; none of this module's
    // fields are unclassified, because CY_TRACE_FIELD has no overload that omits one.
    CY_CHECK_EQ(closed.value().unclassified_fields, 0u);
}

CY_TEST_CASE("the diagnostic reports are a no-op when no trace is open") {
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());
    CY_REQUIRE_FALSE(diag::trace_is_open());

    // A caller should not have to ask first; that is what makes it safe to report at a frame
    // boundary in a build where nobody opened a capture.
    jobs_trace_report(system->stats());
    jobs_trace_critical_path(system->critical_path());
    jobs_log_watchdog("nothing", "no-task", 0, kNotAWorker);
}
