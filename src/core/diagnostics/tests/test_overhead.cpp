// SPDX-License-Identifier: MIT
// The engine reports the cost of its own diagnostics.
//
// `diagnostics-profiling-and-crash` — "Diagnostics overhead": "the engine SHALL be able to report
// the cost of its own diagnostics". `measure_emission_cost()` is that report, and this suite holds
// what the benchmark in ../bench/diag_overhead.cpp relies on: the figures are produced with the
// trace closed and with it recording, the recording figures are of emissions that reached a buffer
// rather than ones the loss policy refused, and the trace closes cleanly afterwards.
//
// It asserts no threshold. The numbers themselves come from `just diagnose-overhead`, which runs
// the benchmark at a sample count large enough to mean something, and the module's README records
// them. A threshold here would be a claim about whichever machine ran the suite.
//
// HOW TO MAKE IT FAIL: report `trace_was_open` as anything but trace_is_open() in
// measure_emission_cost() — the recording figure then claims it was measured closed.

#include <cy/core/diagnostics/trace.h>
#include <cy/test/test.h>

using namespace cy::diag;

namespace {

/// Batches small enough to stay inside the verbose channel's share of a 1 MiB ring: one batch is
/// an instant loop, an instant-with-fields loop and a scope-pair loop, drained before the next.
constexpr u32 kBatch = 2000;
constexpr u32 kBatches = 10;

}  // namespace

CY_TEST_CASE("overhead: the engine measures its own emission cost, closed and recording") {
    CY_REQUIRE_MESSAGE(!trace_is_open(), "no trace is open when the suite starts");

    // Compiled in, turned off: the shipping-with-telemetry-off figure.
    const EmissionCost closed = measure_emission_cost(kBatch);
    CY_CHECK_MESSAGE(!closed.trace_was_open, "the closed figure says it was measured closed");
    CY_CHECK_MESSAGE(closed.samples == kBatch, "and over the samples it was asked for");
    CY_CHECK_MESSAGE(closed.instant_ns > 0.0, "and it is a measurement rather than a zero");

    TraceConfig config;
    config.path = "cy_diag_overhead.cytrace";
    config.consumer_thread = false;
    config.buffer_bytes_per_thread = 1u << 20;
    CY_REQUIRE_MESSAGE(trace_open(config).has_value(), "the trace opens");

    EmissionCost recording{};
    for (u32 batch = 0; batch < kBatches; ++batch) {
        const EmissionCost cost = measure_emission_cost(kBatch);
        recording.trace_was_open = cost.trace_was_open;
        recording.instant_ns += cost.instant_ns;
        recording.instant_fields_ns += cost.instant_fields_ns;
        recording.scope_pair_ns += cost.scope_pair_ns;
        trace_flush();
    }
    const auto stats = trace_close();
    CY_REQUIRE_MESSAGE(stats.has_value(), "the trace closes");

    CY_CHECK_MESSAGE(recording.trace_was_open, "the recording figure says it was measured open");
    CY_CHECK_MESSAGE(recording.instant_ns > 0.0, "an instant has a cost");
    CY_CHECK_MESSAGE(recording.instant_fields_ns > 0.0, "an instant with fields has a cost");
    CY_CHECK_MESSAGE(recording.scope_pair_ns > 0.0, "a scope pair has a cost");

    // Every measured emission reached the ring, so the figures are the cost of emitting and not the
    // cost of being refused: four records per sample per batch.
    const TraceStats& written = stats.value();
    for (const u64 dropped : written.dropped) {
        CY_CHECK_MESSAGE(dropped == 0u, "nothing measured was refused");
    }
    CY_CHECK_MESSAGE(written.events_emitted == u64{kBatches} * kBatch * 4u,
                     "every measured emission was recorded");
}
