// The value layer on the M0 trace. Task 1.3.6. Integration: it opens a trace and writes a file.
//
// `core-type-system` — "Diagnostics" — routes the type system's reporting through
// `core/diagnostics` rather than through anything of its own, and
// `diagnostics-profiling-and-crash` requires one trace with many producers. So what is asserted
// here is that the counters move with the work, that the report reaches the one trace, and that the
// report costs nothing when no trace is open — which is the shipping case.

#include <cy/core/diagnostics/trace.h>
#include <cy/core/values/callable.h>
#include <cy/core/values/diagnostics.h>
#include <cy/core/values/handle.h>
#include <cy/core/values/signal.h>
#include <cy/core/values/var.h>

#include <cy/test/fixtures.h>
#include <cy/test/test.h>

namespace {

CY_HANDLE_TAG(Reported);

cy::Expected<cy::Var, cy::CallError> always_fails(std::span<const cy::Var>) noexcept {
    return cy::call_failed(cy::CallErrorKind::Failed, cy::Name::intern("diag.listener"),
                           "this listener always fails");
}

}  // namespace

CY_TEST_CASE("values_diagnostics: counters follow the work") {
    const cy::values::ValueDiagnostics before = cy::values::values_diagnostics();

    // A stale handle rejection, which is the counter worth watching: it is a symptom of an owner
    // freeing something another system still refers to.
    cy::GenerationTable table(64);
    const cy::Expected<cy::Handle<ReportedTag>, cy::Error> handle =
        table.allocate_handle<ReportedTag>();
    CY_REQUIRE(handle.has_value());
    CY_REQUIRE(table.release(*handle).has_value());
    CY_CHECK_FALSE(table.is_live(*handle));

    // A heap block, and a copy-on-write detach.
    cy::Var array = cy::Var::empty_array();
    CY_REQUIRE((*array.array_mut())->push(cy::Var::from_int(1)).has_value());
    cy::Var copy = array;
    CY_REQUIRE(copy.array_mut().has_value());

    // A signal emission whose listener fails.
    cy::Signal signal(cy::Name::intern("diag.signal"), 0);
    CY_REQUIRE(
        signal.connect(cy::Callable::from_free(cy::Name::intern("diag.listener"), &always_fails))
            .has_value());
    CY_REQUIRE(signal.emit({}).has_value());

    const cy::values::ValueDiagnostics after = cy::values::values_diagnostics();
    CY_CHECK_GT(after.stale_handle_rejections, before.stale_handle_rejections);
    CY_CHECK_GT(after.var_blocks_allocated, before.var_blocks_allocated);
    CY_CHECK_GT(after.var_blocks_detached, before.var_blocks_detached);
    CY_CHECK_GT(after.signal_emissions, before.signal_emissions);
    CY_CHECK_GT(after.call_failures, before.call_failures);
    CY_CHECK_GT(after.names_interned, 1u);
    CY_CHECK_EQ(after.handle_slots_live, after.handle_slots_allocated - after.handle_slots_freed);
}

CY_TEST_CASE("values_trace_report: with no trace open it does nothing and does not crash") {
    CY_REQUIRE_FALSE(cy::diag::trace_is_open());
    cy::values::values_trace_report();
    cy::values::values_log_report();
    cy::values::values_log_call_failure(cy::Name::intern("diag.listener"), "Failed");
    CY_CHECK_FALSE(cy::diag::trace_is_open());
}

CY_TEST_CASE("values_trace_report: the report reaches the one trace") {
    const cy::test::TempDir directory("values_diagnostics");
    CY_REQUIRE(directory.valid());
    const std::string path = directory.file("values.cytrace");

    cy::diag::TraceConfig config;
    config.path = path.c_str();
    config.consumer_thread = false;  // drained on the flush this test performs itself
    config.build_identity = "values-diagnostics-test";

    const cy::Expected<cy::diag::TraceId, cy::Error> opened = cy::diag::trace_open(config);
    CY_REQUIRE(opened.has_value());

    // Some work to report on, then the report.
    const cy::Var value = cy::Var::from_string("reported");
    (void)value.as_string();
    cy::values::values_trace_report();
    cy::values::values_log_report();
    cy::values::values_log_call_failure(cy::Name::intern("diag.listener"), "Failed");

    const cy::Expected<cy::diag::TraceStats, cy::Error> stats = cy::diag::trace_close();
    CY_REQUIRE(stats.has_value());
    CY_CHECK_GT(stats->events_emitted, 0u);
    CY_CHECK_GT(stats->events_written, 0u);
    CY_CHECK_GT(stats->bytes_written, 0u);
    // Every field the report emits is registered with a classification, so none of them is counted
    // as unclassified by the writer. That is the invariant, measured rather than asserted about.
    CY_CHECK_EQ(stats->unclassified_fields, 0u);
}
