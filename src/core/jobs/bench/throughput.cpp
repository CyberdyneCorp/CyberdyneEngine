// The job system's throughput, with a threshold that is checked rather than printed. Task 3.2.12.
//
// `testing-and-quality` requires a benchmark to fail when a metric regresses beyond a per-benchmark
// threshold, and that an intentional trade-off updates the threshold in the same change. So this
// binary exits non-zero on a regression: `just test-bench-jobs` is a gate, not a report.
//
// WHAT IS MEASURED, AND WHY EACH ONE IS A RATIO. A nanosecond figure is a property of the machine
// that recorded it, and this runs on developer laptops and on a shared CI runner. Every metric here
// is therefore dimensionless — a speedup, or a cost expressed in units of a calibration workload
// measured in the same process — which is the same position benchmarks/tools/compare.py takes for
// the shared harness and for the same reason.
//
//   jobs/parallel-efficiency      the speedup divided by the worker count. Not the raw speedup: a
//                                 twenty-four-core workstation and a two-core CI runner have
//                                 different speedups for the same correct scheduler, and a
//                                 threshold on the raw number would gate the hardware rather than
//                                 the engine. Regression: the scheduler lost parallelism.
//   jobs/parallel-for-efficiency  the same, through submit_parallel_for, which is the path every
//                                 system will use. Regression: partitioning, or the join.
//   jobs/dispatch-overhead        the cost of submitting and running one empty task, in units of
//                                 the calibration workload. Regression: submission got more
//                                 expensive — a lock got hotter, or a wakeup got slower.
//   jobs/scheduling-allocations   general-heap allocations the scheduler made after `start()`,
//                                 across the whole run. Exactly zero, on every machine, always —
//                                 task records come from per-participant slabs and a task's
//                                 arguments travel inside the record. This is the one metric here
//                                 that is not a measurement at all but an invariant, and it is the
//                                 one that will catch the regression that matters most: somebody
//                                 putting a std::vector or a std::function on the submission path.
//
// The calibration workload is a fixed integer loop, measured in this process immediately before the
// rest. A machine twice as fast reports half the nanoseconds for everything including the
// calibration, so the ratios hold.
//
// Thresholds live in thresholds.json beside this file and are committed. `--record` rewrites them,
// which is a reviewed step: a threshold a failing run could rewrite for itself would gate nothing.

#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/parallel.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace cy;
using namespace cy::jobs;

namespace {

/// How much arithmetic one unit of work is. Large enough that a task is worth scheduling and small
/// enough that ten thousand of them fit in a benchmark.
constexpr u64 kWorkIterations = 4000;
constexpr u64 kTaskCount = 20'000;
constexpr u64 kCalibrationUnits = 20'000;
/// How many passes to take. Each pass measures everything back to back and yields one set of
/// ratios; the reported figure is the best pass. A benchmark on a shared runner competes with
/// whatever else is on the machine, and the best pass is the one least contaminated by it.
constexpr u32 kRepeats = 7;

/// One unit of work. Written so the optimiser cannot delete it and cannot hoist it out of a loop.
u64 work_unit(u64 seed) noexcept {
    u64 value = seed | 1;
    for (u64 i = 0; i < kWorkIterations; ++i) {
        value = (value * 6364136223846793005ull) + 1442695040888963407ull;
        value ^= value >> 29;
    }
    return value;
}

void work_task(const TaskContext& context, void*) noexcept {
    // The index is the task's sequence, which differs per task, so no two tasks compute the same
    // thing and the compiler cannot share the result.
    const u64 result = work_unit(context.self.bits());
    static std::atomic<u64> sink{0};
    sink.fetch_add(result, std::memory_order_relaxed);
}

void empty_task(const TaskContext&, void*) noexcept {}

f64 nanoseconds_since(i64 started) noexcept {
    return static_cast<f64>(monotonic_now_ns() - started);
}

/// One pass over every measurement, taken back to back.
struct Sample {
    f64 calibration_ns = 0.0;
    f64 serial_ns = 0.0;
    f64 parallel_ns = 0.0;
    f64 parallel_for_ns = 0.0;
    f64 dispatch_ns = 0.0;
};

f64 measure_calibration() noexcept {
    const i64 started = monotonic_now_ns();
    static std::atomic<u64> sink{0};
    u64 total = 0;
    for (u64 i = 0; i < kCalibrationUnits; ++i) {
        total += work_unit(i);
    }
    sink.store(total, std::memory_order_relaxed);
    return nanoseconds_since(started) / static_cast<f64>(kCalibrationUnits);
}

f64 measure_serial() noexcept {
    const i64 started = monotonic_now_ns();
    static std::atomic<u64> sink{0};
    u64 total = 0;
    for (u64 i = 0; i < kTaskCount; ++i) {
        total += work_unit(i);
    }
    sink.store(total, std::memory_order_relaxed);
    return nanoseconds_since(started);
}

f64 measure_parallel(JobSystem& jobs) noexcept {
    const i64 started = monotonic_now_ns();
    for (u64 i = 0; i < kTaskCount; ++i) {
        if (!jobs.submit(&work_task, nullptr, "bench.work")) {
            jobs.wait_for_idle();
        }
    }
    jobs.wait_for_idle();
    return nanoseconds_since(started);
}

f64 measure_parallel_for(JobSystem& jobs) noexcept {
    const i64 started = monotonic_now_ns();
    auto handle = jobs.submit_parallel_for(
        kTaskCount, 64,
        [](const TaskContext&, u64 begin, u64 end, void*) noexcept {
            static std::atomic<u64> sink{0};
            u64 total = 0;
            for (u64 i = begin; i < end; ++i) {
                total += work_unit(i);
            }
            sink.fetch_add(total, std::memory_order_relaxed);
        },
        nullptr, "bench.parallel_for");
    if (handle) {
        jobs.wait(handle.value());
    }
    return nanoseconds_since(started);
}

/// Submission and completion of an empty task: the scheduling cost with the work removed.
f64 measure_dispatch(JobSystem& jobs) noexcept {
    const i64 started = monotonic_now_ns();
    for (u64 i = 0; i < kTaskCount; ++i) {
        if (!jobs.submit(&empty_task, nullptr, "bench.empty")) {
            jobs.wait_for_idle();
        }
    }
    jobs.wait_for_idle();
    return nanoseconds_since(started) / static_cast<f64>(kTaskCount);
}

/// Every measurement in one pass, taken back to back so that no pass straddles a change in what
/// else the machine is doing.
///
/// The reported figure for each quantity is its MINIMUM over the passes, and the ratios are formed
/// from those minima. A minimum is the standard estimator here because contention can only ever
/// make a measurement larger: the smallest time observed is the closest this process got to having
/// the machine to itself, and it is the number a threshold should be compared against.
///
/// The tempting alternative — taking the best *ratio* over passes — is worse, and subtly: it
/// rewards the pass whose serial baseline was slowest, so a moment of contention during the serial
/// run inflates the reported speedup rather than deflating it. Measured that way this benchmark
/// reported a parallel efficiency above one, which is not a thing.
Sample measure_once(JobSystem& jobs) noexcept {
    Sample sample;
    sample.calibration_ns = measure_calibration();
    sample.serial_ns = measure_serial();
    sample.parallel_ns = measure_parallel(jobs);
    sample.parallel_for_ns = measure_parallel_for(jobs);
    sample.dispatch_ns = measure_dispatch(jobs);
    return sample;
}

// --- The thresholds ---------------------------------------------------------------------------
//
// A deliberately small reader for a file this repository owns and whose shape it fixes: find
// `"name"`, skip to the `:` after it, read a number. Pulling in a JSON library to read six numbers
// would be a dependency added for a benchmark, which `thirdparty-dependencies` would rightly ask
// about.

struct Metric {
    const char* name;
    /// True when a *larger* value is better — an efficiency. False for a cost.
    bool higher_is_better;
    /// How far the recorded value may move before the run fails, as a fraction of it.
    ///
    /// Wide, and deliberately so. These numbers are measured on whatever machine happens to run
    /// them, against whatever else is running on it: a developer workstation mid-build reports half
    /// the parallel efficiency and twice the dispatch cost of the same code on an idle one. A
    /// margin narrow enough to catch a ten-per-cent regression would fail on a busy laptop within a
    /// week and be switched off, which is worse than a wide one that holds. What this gate catches
    /// is the order-of-magnitude kind: parallelism lost, an allocation on the scheduling path, a
    /// lock that became contended. A tighter threshold is possible and needs a dedicated runner.
    ///
    /// Per metric, because they are not equally noisy: a parallel efficiency degrades roughly with
    /// the machine's load, and a dispatch cost is dominated by cross-thread wakeup latency, which
    /// degrades faster.
    f64 margin;
    f64 measured;
    f64 threshold;
    bool have_threshold;
};

bool read_number(const char* text, const char* key, f64& out) noexcept {
    char quoted[128];
    std::snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char* found = std::strstr(text, quoted);
    if (found == nullptr) {
        return false;
    }
    const char* colon = std::strchr(found + std::strlen(quoted), ':');
    if (colon == nullptr) {
        return false;
    }
    char* end = nullptr;
    const f64 value = std::strtod(colon + 1, &end);
    if (end == colon + 1) {
        return false;
    }
    out = value;
    return true;
}

char* read_file(const char* path) noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return nullptr;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return nullptr;
    }
    // calloc rather than malloc, for the extra byte: the buffer arrives zero-filled, so a short
    // read is already terminated and there is no indexed write to terminate it with. That is one
    // fewer bound to reason about — the alternative spells `buffer[read] = 0`, whose index the
    // reader, and the static analyser, must derive from fread's contract.
    auto* buffer = static_cast<char*>(std::calloc(static_cast<usize>(size) + 1, 1));
    if (buffer == nullptr) {
        std::fclose(file);
        return nullptr;
    }
    const usize read = std::fread(buffer, 1, static_cast<usize>(size), file);
    std::fclose(file);
    if (read == 0) {
        std::free(buffer);
        return nullptr;
    }
    return buffer;
}

int write_thresholds(const char* path, const Metric* metrics, u32 count, f64 calibration) noexcept {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "throughput: could not write %s\n", path);
        return 2;
    }
    std::fprintf(
        file,
        "{\n"
        "  \"note\": \"Thresholds for the job system's throughput benchmark (task "
        "3.2.12). Every metric is dimensionless so that it survives being run on a "
        "different machine: a speedup, or a cost in units of the calibration workload "
        "measured in the same process. A speedup must not fall below its threshold; a cost "
        "must not rise above it. Rewritten by `just test-bench-jobs --record`, which is a "
        "reviewed step: a threshold a failing run could rewrite for itself would gate "
        "nothing.\",\n"
        "  \"calibration_ns_per_unit_when_recorded\": %.4f,\n"
        "  \"metrics\": {\n",
        calibration);
    for (u32 i = 0; i < count; ++i) {
        // The threshold is the measurement moved by the metric's own margin, so that ordinary
        // run-to-run noise on a shared runner is not a failure while a real regression still is.
        const f64 recorded = metrics[i].higher_is_better
                                 ? metrics[i].measured * (1.0 - metrics[i].margin)
                                 : metrics[i].measured * (1.0 + metrics[i].margin);
        std::fprintf(file, "    \"%s\": %.4f%s\n", metrics[i].name, recorded,
                     i + 1 == count ? "" : ",");
    }
    std::fprintf(file, "  }\n}\n");
    std::fclose(file);
    std::printf("throughput: thresholds rewritten to %s\n", path);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool record = false;
    const char* path = CY_JOBS_BENCH_THRESHOLDS;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--record") == 0) {
            record = true;
        } else if (std::strcmp(argv[i], "--thresholds") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--record] [--thresholds <file>]\n", argv[0]);
            return 2;
        }
    }

    JobSystem jobs;
    JobSystemConfig config;
    config.task_slots_per_participant = 8192;
    config.deque_capacity = 8192;
    if (auto started = jobs.start(config); !started) {
        std::fprintf(stderr, "throughput: the job system did not start: %s\n",
                     started.error().message);
        return 2;
    }
    const u32 workers = jobs.worker_count();
    const f64 scale = workers > 0 ? static_cast<f64>(workers) : 1.0;

    Sample best;
    for (u32 pass = 0; pass < kRepeats; ++pass) {
        const Sample sample = measure_once(jobs);
        if (pass == 0) {
            best = sample;
            continue;
        }
        best.calibration_ns = sample.calibration_ns < best.calibration_ns ? sample.calibration_ns
                                                                          : best.calibration_ns;
        best.serial_ns = sample.serial_ns < best.serial_ns ? sample.serial_ns : best.serial_ns;
        best.parallel_ns =
            sample.parallel_ns < best.parallel_ns ? sample.parallel_ns : best.parallel_ns;
        best.parallel_for_ns = sample.parallel_for_ns < best.parallel_for_ns
                                   ? sample.parallel_for_ns
                                   : best.parallel_for_ns;
        best.dispatch_ns =
            sample.dispatch_ns < best.dispatch_ns ? sample.dispatch_ns : best.dispatch_ns;
    }

    const JobSystemStats stats = jobs.stats();
    jobs.shutdown();

    const f64 calibration = best.calibration_ns;
    const f64 serial = best.serial_ns;
    const f64 parallel = best.parallel_ns;
    const f64 parallel_for = best.parallel_for_ns;
    const f64 dispatch = best.dispatch_ns;

    Metric metrics[] = {
        {"jobs/parallel-efficiency", true, 0.70, (serial / parallel) / scale, 0.0, false},
        {"jobs/parallel-for-efficiency", true, 0.70, (serial / parallel_for) / scale, 0.0, false},
        {"jobs/dispatch-overhead", false, 1.50, dispatch / calibration, 0.0, false},
        {"jobs/scheduling-allocations", false, 0.0, static_cast<f64>(stats.scheduling_allocations),
         0.0, false},
    };
    constexpr u32 kMetricCount = sizeof(metrics) / sizeof(metrics[0]);

    std::printf("job system throughput\n");
    std::printf("  workers                 %u\n", workers);
    std::printf("  calibration             %.2f ns per work unit\n", calibration);
    std::printf("  serial                  %.2f ms for %llu units\n", serial / 1e6,
                static_cast<unsigned long long>(kTaskCount));
    std::printf("  parallel (submit)       %.2f ms  (speedup %.2fx)\n", parallel / 1e6,
                serial / parallel);
    std::printf("  parallel (parallel_for) %.2f ms  (speedup %.2fx)\n", parallel_for / 1e6,
                serial / parallel_for);
    std::printf("  dispatch                %.0f ns per empty task\n", dispatch);
    std::printf("  scheduling allocations  %llu\n",
                static_cast<unsigned long long>(stats.scheduling_allocations));
    std::printf("  steal successes         %llu of %llu attempts\n",
                static_cast<unsigned long long>(stats.steal_successes),
                static_cast<unsigned long long>(stats.steal_attempts));

    if (record) {
        return write_thresholds(path, metrics, kMetricCount, calibration);
    }

    char* text = read_file(path);
    if (text == nullptr) {
        std::fprintf(stderr,
                     "throughput: no thresholds at %s. A number with no budget is a measurement, "
                     "not a gate — record one with `just test-bench-jobs --record`.\n",
                     path);
        return 1;
    }
    for (auto& metric : metrics) {
        metric.have_threshold = read_number(text, metric.name, metric.threshold);
    }
    std::free(text);

    int status = 0;
    std::printf("\nthresholds  %s\n", path);
    for (const auto& metric : metrics) {
        if (!metric.have_threshold) {
            std::fprintf(stderr, "  %-26s %.4f — no threshold recorded\n", metric.name,
                         metric.measured);
            status = 1;
            continue;
        }
        const bool passed = metric.higher_is_better ? metric.measured >= metric.threshold
                                                    : metric.measured <= metric.threshold;
        std::printf("  %-26s %.4f against %s %.4f  %s\n", metric.name, metric.measured,
                    metric.higher_is_better ? "a floor of" : "a ceiling of", metric.threshold,
                    passed ? "ok" : "REGRESSED");
        if (!passed) {
            status = 1;
        }
    }

    if (status != 0) {
        std::fprintf(stderr,
                     "\nthroughput: a threshold was crossed. Either the change is a regression, or "
                     "it is a trade-off — in which case re-record the threshold in the same change "
                     "with `just test-bench-jobs --record` and say why in the commit.\n");
    }
    return status;
}
