#pragma once
// Breadcrumbs: durable markers of what the process was doing.
//
// `diagnostics-profiling-and-crash` — "Breadcrumbs": a bounded, low-cost mechanism, distinct from
// the verbose trace, precisely because they must survive when the trace does not. A breadcrumb is
// written into a fixed global ring that no drain empties and no loss policy discards, so the crash
// handler can read it from a damaged process without touching the trace's buffers, its consumer
// thread, or its file.
//
// A breadcrumb is also emitted onto the trace as an EventKind::Breadcrumb record on the critical
// channel, so a capture that survives shows the same markers in context. The ring is the copy that
// is guaranteed.

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/prelude.h>

namespace cy::diag {

/// Fixed and small: coarse phase boundaries only — tick, stage, asset activation, level transition,
/// save, and the render graph's per-pass markers when M3 adds them.
inline constexpr u32 kBreadcrumbCapacity = 64;

struct Breadcrumb {
    u64 sequence = 0;  // monotonically increasing; a gap means the ring wrapped
    u64 timestamp_ns = 0;
    u64 detail = 0;  // the tick, the pass index, the asset id — whatever the phase counts
    NameId phase = kInvalidName;
};

/// Record a breadcrumb. No allocation, no lock, callable from any thread.
void breadcrumb(NameId phase, u64 detail) noexcept;

/// Copy the ring, oldest first, into `out`. Async-signal-safe: it reads a fixed array and takes no
/// lock, which is why the crash handler may call it.
u32 breadcrumb_snapshot(Breadcrumb* out, u32 capacity) noexcept;

}  // namespace cy::diag

/// Record a breadcrumb whose phase is a literal, registered once at the site.
#define CY_BREADCRUMB(literal, detail)                                                             \
    do {                                                                                           \
        static const ::cy::diag::NameId cy_breadcrumb_phase_ = ::cy::diag::register_name(literal); \
        ::cy::diag::breadcrumb(cy_breadcrumb_phase_, (detail));                                    \
    } while (false)
