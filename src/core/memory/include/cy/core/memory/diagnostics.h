#pragma once
// Memory diagnostics: what every build reports, and where it goes. Task 2.11.
//
// `core-memory-and-containers` — "Memory diagnostics". ALL BUILDS provide per-domain live and peak
// bytes, budget utilisation, pressure level and its history, retirement queue depths, and pool and
// arena utilisation. DEVELOPMENT BUILDS add per-tag live bytes, peak bytes and allocation counts,
// leak reporting at shutdown with the allocating call site, red zones, poisoning, and double-free
// validation — which is `TrackingAllocator` (tracking_allocator.h), reached from here.
//
// Allocation and free events, pressure transitions and budget violations are emitted into the
// SHARED TRACE that `diagnostics-profiling-and-crash` defines, so a memory spike correlates with
// the frame, task, asset and streaming activity that caused it. There is no memory timeline; there
// is the timeline, with memory on it.
//
// TELEMETRY EXISTS BEFORE ALLOCATOR OPTIMISATION. That ordering is a requirement of the
// specification rather than advice, and it is why this header and `domain.h` were written before
// the general-heap measurement in `README.md` was run: choosing an allocator without per-domain
// attribution is guesswork, and the numbers in that measurement come from these counters.

#include <cy/core/base/types.h>
#include <cy/core/memory/budget.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/epoch.h>
#include <cy/core/memory/pressure.h>

namespace cy {

/// Everything a report needs, in one snapshot taken at one moment.
struct MemoryDiagnostics {
    /// Per domain, including descendants — the figure a budget is compared against.
    DomainStats domains[kMemoryDomainCount] = {};
    u64 total_live_bytes = 0;
    u64 total_peak_bytes = 0;
    u64 total_reserved_bytes = 0;

    PressureLevel pressure = PressureLevel::Normal;
    u64 pressure_transitions = 0;

    u32 budgeted_domains = 0;
    u32 over_budget_domains = 0;
    /// The domain whose utilisation is highest, and what it is.
    MemoryDomain worst_domain = MemoryDomain::Engine;
    f64 worst_utilisation = 0.0;

    RetirementStats retirement;

    u64 process_lifetime_bytes = 0;
    u32 process_lifetime_allocations = 0;

    u32 allocator_scope_depth = 0;
    u64 allocator_scope_overflows = 0;
};

[[nodiscard]] MemoryDiagnostics memory_diagnostics() noexcept;

/// Emit the snapshot onto the shared trace: one counter series per figure that moves, and one
/// instant carrying the whole snapshot as classified fields. A no-op when no trace is open.
void memory_trace_report() noexcept;

/// Emit the budget tree as a log record per budgeted domain, target against actual. What
/// `just diagnose-memory` and the headless sample print.
void memory_log_report() noexcept;

/// Record a pressure transition on the trace. Called by whoever drives `update_memory_pressure()`;
/// separate from the monitor itself so that `core-memory` does not have to depend on the trace in a
/// header, and so a subsystem can record a transition it forced.
void memory_trace_pressure(PressureLevel level, PressureLevel previous, MemoryDomain cause,
                           f64 utilisation) noexcept;

/// Record that a hard budget refused a growth, or that a cache evicted rather than allocate. The
/// "the eviction SHALL be reported" half of the budget requirement.
void memory_trace_budget_violation(MemoryDomain which, u64 wanted, u64 allowance) noexcept;
void memory_trace_eviction(MemoryDomain which, u64 bytes) noexcept;

/// Write the process's leak report to the log: one record per leaked allocation with its tag, size
/// and call site, and a summary. Allocations declared through `lifetime.h` are counted as
/// intentional and excluded from the leaked figures — see the note in that header.
///
/// `tracker` may be null, in which case only the domain-level and process-lifetime figures are
/// reported: an engine built without a tracking allocator still says how much it is holding.
class TrackingAllocator;
void memory_log_leak_report(const TrackingAllocator* tracker) noexcept;

}  // namespace cy
