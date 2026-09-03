// What diagnostics cost, as a number.
//
// `diagnostics-profiling-and-crash` — "Diagnostics overhead": overhead is bounded and declared —
// shipping with minimal telemetry well under one per cent of frame time, development with normal
// tracing a small single-digit percentage — and the engine can report the cost of its own
// diagnostics. A claim without a measurement is not a result, so this measures the four states that
// matter and prints the per-frame cost each implies at a 60 Hz budget.
//
// Task 3.5.9. Run by `just diagnose-overhead`; its numbers are recorded in this module's README.

#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace cy::diag;

namespace {

CY_TRACE_CATEGORY(bench_category, "bench")
CY_TRACE_NAME(bench_event, "bench.event")
CY_TRACE_FIELD(bench_index, u64, cy::Privacy::Public)
CY_TRACE_FIELD(bench_value, f64, cy::Privacy::Public)
CY_LOG_CATEGORY(bench_log, "bench")

constexpr f64 kFrameBudgetNs = 16'666'667.0;      // 60 Hz
constexpr u64 kEventsPerFrameShipping = 200;      // breadcrumbs, ticks, counters
constexpr u64 kEventsPerFrameDevelopment = 5000;  // normal tracing

u64 now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void report(const char* label, const EmissionCost& cost, u64 events_per_frame) {
    const f64 per_frame = cost.instant_ns * static_cast<f64>(events_per_frame);
    std::printf("%-28s %9.1f %9.1f %9.1f   %8.1f %8.3f%%\n", label, cost.instant_ns,
                cost.instant_fields_ns, cost.scope_pair_ns, per_frame,
                100.0 * per_frame / kFrameBudgetNs);
}

/// Measure in batches, draining between them, so that every emission the figures average over
/// actually reached a buffer. Measuring a producer that is being refused by the loss policy would
/// report the cost of the refusal — which is real, and is not what "the cost of emitting" means.
EmissionCost measure_in_batches(u32 batches, u32 samples_per_batch) {
    EmissionCost total{};
    for (u32 batch = 0; batch < batches; ++batch) {
        const EmissionCost cost = measure_emission_cost(samples_per_batch);
        total.instant_ns += cost.instant_ns;
        total.instant_fields_ns += cost.instant_fields_ns;
        total.scope_pair_ns += cost.scope_pair_ns;
        total.trace_was_open = cost.trace_was_open;
        trace_flush();
    }
    const f64 divisor = static_cast<f64>(batches);
    total.samples = u64{batches} * samples_per_batch;
    total.instant_ns /= divisor;
    total.instant_fields_ns /= divisor;
    total.scope_pair_ns /= divisor;
    return total;
}

/// The floor: what the loop itself costs without any emission in it, so the figures above are the
/// cost of diagnostics rather than the cost of a for-loop.
f64 measure_empty_loop(u32 samples) {
    volatile u64 sink = 0;
    const u64 start = now_ns();
    for (u32 index = 0; index < samples; ++index) {
        sink = sink + index;
    }
    const u64 elapsed = now_ns() - start;
    return static_cast<f64>(elapsed) / static_cast<f64>(samples);
}

}  // namespace

int main(int argc, char** argv) {
    const u32 samples =
        (argc > 1) ? static_cast<u32>(std::strtoul(argv[1], nullptr, 10)) : 1'000'000;
    const char* path = (argc > 2) ? argv[2] : "cy_diag_bench.cytrace";

    std::printf("diagnostics overhead — %u samples per measurement, 60 Hz budget %.2f ms\n\n",
                samples, kFrameBudgetNs / 1e6);
    std::printf("%-28s %9s %9s %9s   %8s %9s\n", "state", "instant", "+2 fields", "scope",
                "ns/frame", "of frame");
    std::printf("%-28s %9s %9s %9s   %8s %9s\n", "----------------------------", "---------",
                "---------", "---------", "--------", "---------");

    std::printf("empty loop iteration: %.2f ns\n", measure_empty_loop(samples));

    // 1. Compiled in, turned off: no trace open. This is the shipping-with-telemetry-off number.
    report("trace closed", measure_emission_cost(samples), kEventsPerFrameShipping);

    // 2. Open, and actually recording. A batch is sized to stay inside the verbose channel's share
    // of the buffer, and the buffer is drained between batches, so nothing here is refused.
    TraceConfig config;
    config.path = path;
    config.consumer_thread = false;
    config.buffer_bytes_per_thread = 1u << 20;
    if (!trace_open(config)) {
        std::fprintf(stderr, "the trace did not open\n");
        return 1;
    }
    // Sized so that one batch — an instant loop, an instant-with-fields loop and a scope-pair loop
    // — stays inside the verbose channel's share of the buffer, so nothing is refused
    // mid-measurement.
    constexpr u32 kBatch = 2000;
    const u32 batches = (samples / kBatch) > 0 ? (samples / kBatch) : 1;
    const EmissionCost open_cost = measure_in_batches(batches, kBatch);
    report("open, recording", open_cost, kEventsPerFrameShipping);
    report("open, development volume", open_cost, kEventsPerFrameDevelopment);

    // 3. A log record that the level filter rejects: the check and nothing else.
    set_log_level(LogLevel::Warning);
    const u64 start = now_ns();
    for (u32 index = 0; index < samples; ++index) {
        CY_LOG(bench_log(), LogLevel::Debug, "bench.filtered", field_u64(bench_index(), index));
    }
    const f64 filtered_ns = static_cast<f64>(now_ns() - start) / static_cast<f64>(samples);
    std::printf("%-28s %9.1f\n", "filtered log record", filtered_ns);

    const auto closed = trace_close();
    if (!closed) {
        std::fprintf(stderr, "the trace did not close\n");
        return 1;
    }
    const TraceStats stats = closed.value();
    std::printf(
        "\nwritten: %llu events, %llu bytes; dropped %llu/%llu/%llu/%llu "
        "(critical/important/verbose/sampled)\n",
        static_cast<unsigned long long>(stats.events_written),
        static_cast<unsigned long long>(stats.bytes_written),
        static_cast<unsigned long long>(stats.dropped[0]),
        static_cast<unsigned long long>(stats.dropped[1]),
        static_cast<unsigned long long>(stats.dropped[2]),
        static_cast<unsigned long long>(stats.dropped[3]));

    // 4. With a consumer thread running, which is how a session actually runs.
    TraceConfig with_consumer = config;
    with_consumer.consumer_thread = true;
    with_consumer.drain_interval_ms = 2;
    if (!trace_open(with_consumer)) {
        return 1;
    }
    report("open, consumer draining", measure_in_batches(batches, kBatch), kEventsPerFrameShipping);
    const auto drained = trace_close();
    if (drained) {
        const TraceStats final_stats = drained.value();
        std::printf("written: %llu events, %llu bytes; dropped %llu\n",
                    static_cast<unsigned long long>(final_stats.events_written),
                    static_cast<unsigned long long>(final_stats.bytes_written),
                    static_cast<unsigned long long>(final_stats.dropped[2]));
    }
    return 0;
}
