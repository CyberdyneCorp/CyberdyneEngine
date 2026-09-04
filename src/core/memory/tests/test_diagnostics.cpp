// The memory layer's report, on the M0 trace. Task 2.11.
//
// An integration test: it opens the one trace, writes an artefact and reads its statistics back.
// The point is that the memory figures land on the timeline every other subsystem writes to, so
// that a memory spike can be read against the frame, task and asset activity around it — not that
// the numbers exist, which the unit suite already checks.

#include <cy/test/test.h>

#include <cy/core/diagnostics/trace.h>
#include <cy/core/memory/budget.h>
#include <cy/core/memory/diagnostics.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/epoch.h>
#include <cy/core/memory/pressure.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/memory/tracking_allocator.h>

#include <cstdio>
#include <string>

namespace {

/// Open a trace over a temporary path, run `body`, close it and return what it recorded.
template <class Body>
cy::diag::TraceStats with_trace(const char* name, Body&& body) {
    std::string path = "cy_memory_";
    path += name;
    path += ".cytrace";

    cy::diag::TraceConfig config;
    config.path = path.c_str();
    config.consumer_thread =
        false;  // the test drains on its own flush, so nothing is timing-dependent
    config.build_identity = "memory-diagnostics-test";
    const auto opened = cy::diag::trace_open(config);
    CY_REQUIRE(opened.has_value());

    body();

    cy::diag::trace_flush();
    const auto stats = cy::diag::trace_close();
    CY_REQUIRE(stats.has_value());
    (void)std::remove(path.c_str());
    return *stats;
}

}  // namespace

CY_TEST_CASE("the memory report lands on the shared trace, with classified fields") {
    const cy::diag::TraceStats stats = with_trace("report", [] {
        cy::memory_trace_report();
        cy::memory_trace_report();
    });

    // One instant plus one counter per domain and per total, twice over.
    CY_CHECK(stats.events_emitted >= 2u * (cy::kMemoryDomainCount + 4u + 1u));
    CY_CHECK(stats.events_written > 0u);
    CY_CHECK_EQ(stats.dropped[0], 0u);
    // Every field this module emits is declared with CY_TRACE_FIELD, so none of them reaches the
    // writer without a classification.
    CY_CHECK_EQ(stats.unclassified_fields, 0u);
}

CY_TEST_CASE(
    "a pressure transition and a budget violation are on the timeline, not only in a counter") {
    const cy::diag::TraceStats stats = with_trace("pressure", [] {
        cy::memory_trace_pressure(cy::PressureLevel::Critical, cy::PressureLevel::Elevated,
                                  cy::MemoryDomain::Streaming, 0.97);
        cy::memory_trace_budget_violation(cy::MemoryDomain::Streaming, 1024, 512);
        cy::memory_trace_eviction(cy::MemoryDomain::Assets, 4096);
    });

    CY_CHECK_EQ(stats.events_emitted, 3u);
    CY_CHECK_EQ(stats.events_written, 3u);
    CY_CHECK_EQ(stats.unclassified_fields, 0u);
}

CY_TEST_CASE("emitting with no trace open is a no-op rather than a failure") {
    CY_REQUIRE_FALSE(cy::diag::trace_is_open());
    cy::memory_trace_report();
    cy::memory_trace_eviction(cy::MemoryDomain::Assets, 1);
    cy::memory_log_report();
    cy::memory_log_leak_report(nullptr);
    CY_CHECK_FALSE(cy::diag::trace_is_open());
}

CY_TEST_CASE("the snapshot reports what every build must be able to answer") {
    cy::BudgetTree& budgets = cy::default_budget_tree();
    const cy::MemoryProfile* desktop = cy::find_memory_profile("desktop");
    CY_REQUIRE(desktop != nullptr);
    CY_REQUIRE(budgets.apply(*desktop).has_value());

    cy::SystemAllocator& assets = cy::system_allocator(cy::MemoryDomain::Assets);
    void* block = assets.allocate(cy::usize{64} * 1024);
    CY_REQUIRE(block != nullptr);

    const cy::MemoryDiagnostics snapshot = cy::memory_diagnostics();

    CY_CHECK(snapshot.total_live_bytes >= 64u * 1024u);
    CY_CHECK(snapshot.total_peak_bytes >= snapshot.total_live_bytes);
    CY_CHECK_EQ(snapshot.budgeted_domains, 14u);  // the desktop profile budgets every domain
    CY_CHECK_EQ(snapshot.over_budget_domains, 0u);
    CY_CHECK(snapshot.domains[static_cast<cy::u32>(cy::MemoryDomain::Assets)].live_bytes >=
             64u * 1024u);
    CY_CHECK_EQ(snapshot.retirement.capacity, cy::default_epoch_manager().stats().capacity);

    assets.deallocate(block, cy::usize{64} * 1024);
    budgets.clear();
}

CY_TEST_CASE("a leak report distinguishes a declared lifetime from a defect") {
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::Engine, "report");
    void* leaked = tracker.allocate(512, 16);
    CY_REQUIRE(leaked != nullptr);

    const cy::diag::TraceStats stats =
        with_trace("leaks", [&] { cy::memory_log_leak_report(&tracker); });
    // The leak report is two log records — one for the allocation, one for the summary — and a log
    // record is an event on the same timeline.
    CY_CHECK(stats.events_emitted >= 2u);

    tracker.deallocate(leaked, 512, 16);
}
