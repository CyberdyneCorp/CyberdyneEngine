// The memory layer's diagnostics, emitted onto the M0 trace. Task 2.11.
//
// One category, `memory`, and a field per figure. Every field is declared with CY_TRACE_FIELD,
// which takes its privacy classification as a required third argument — there is no overload
// without one.
//
// THE CLASSIFICATIONS, AND WHY. A byte count, a level and a domain name are Public: they are
// numbers and enumerator names about the engine, and nothing about them identifies a person or a
// machine. `allocation_site` is the exception and is Sensitive: it is a source path from the
// machine that built the binary, which is a filesystem path, and
// `diagnostics-profiling-and-crash` classifies a path as a field rather than letting it become an
// event name. It appears in a developer's own capture and is redacted from anything prepared to
// leave the machine.

#include <cy/core/memory/diagnostics.h>

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>
#include <cy/core/memory/lifetime.h>
#include <cy/core/memory/scope.h>
#include <cy/core/memory/tracking_allocator.h>

#include <cstdio>

namespace cy {
namespace {

CY_TRACE_CATEGORY(category, "memory")

CY_TRACE_FIELD(domain, string, cy::Privacy::Public)
CY_TRACE_FIELD(live_bytes, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(peak_bytes, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(reserved_bytes, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(budget_bytes, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(wanted_bytes, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(evicted_bytes, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(utilisation, f64, cy::Privacy::Public)
CY_TRACE_FIELD(live_allocations, u64, cy::Privacy::Public)
CY_TRACE_FIELD(pressure_level, string, cy::Privacy::Public)
CY_TRACE_FIELD(previous_level, string, cy::Privacy::Public)
CY_TRACE_FIELD(pressure_transitions, u64, cy::Privacy::Public)
CY_TRACE_FIELD(retirement_depth, u64, cy::Privacy::Public)
CY_TRACE_FIELD(retirement_refused, u64, cy::Privacy::Public)
CY_TRACE_FIELD(stalled_epochs, u64, cy::Privacy::Public)
// Named `held_bytes` rather than `process_lifetime_bytes` because the field accessor a
// CY_TRACE_FIELD declares would otherwise be an ambiguous overload against
// cy::process_lifetime_bytes() from lifetime.h, which this file also calls.
CY_TRACE_FIELD(held_bytes, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(leaked_allocations, u64, cy::Privacy::Public)
CY_TRACE_FIELD(leaked_bytes, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(allocation_tag, string, cy::Privacy::Public)

// A source path from the machine that built the binary. See the file comment.
CY_TRACE_FIELD(allocation_site, string, cy::Privacy::Sensitive)

CY_LOG_CATEGORY(log_category, "memory")

[[nodiscard]] diag::FieldValue text_field(diag::FieldId field, const char* text) noexcept {
    u32 length = 0;
    while (text[length] != '\0' && length < diag::kMaxTextBytesPerRecord) {
        ++length;
    }
    return diag::field_text(field, text, length);
}

/// Emit one figure as a Counter record. Verbose: a counter snapshot is background detail, and under
/// buffer pressure it is exactly what should be dropped before a tick boundary is.
void emit_counter(const char* name, u64 value) noexcept {
    const diag::NameId id = diag::register_name(name);
    diag::trace_counter(id, category(), diag::Channel::Verbose, value);
}

/// A per-domain counter name, built once per domain and kept. Registering a name is idempotent, but
/// composing the string on every report would be a formatting cost on a path that must not have
/// one, so the composed names live here for the process.
const char* domain_counter_name(MemoryDomain which) noexcept {
    static char storage[kMemoryDomainCount][40];
    const u32 index = static_cast<u32>(which);
    if (storage[index][0] == '\0') {
        std::snprintf(storage[index], sizeof(storage[index]), "memory.%s.live_bytes",
                      domain_name(which));
    }
    return storage[index];
}

}  // namespace

MemoryDiagnostics memory_diagnostics() noexcept {
    const BudgetTree& budgets = default_budget_tree();
    const PressureMonitor& monitor = default_pressure_monitor();

    MemoryDiagnostics out;
    for (u32 index = 0; index < kMemoryDomainCount; ++index) {
        const auto which = static_cast<MemoryDomain>(index);
        out.domains[index] = domain_stats_recursive(which);
        if (budgets.has_budget(which)) {
            ++out.budgeted_domains;
            if (budgets.over_budget(which)) {
                ++out.over_budget_domains;
            }
        }
    }

    const DomainStats engine = domain_stats_recursive(MemoryDomain::Engine);
    out.total_live_bytes = engine.live_bytes;
    out.total_peak_bytes = engine.peak_bytes;
    out.total_reserved_bytes = engine.reserved_bytes;

    out.pressure = monitor.level();
    out.pressure_transitions = monitor.transition_count();
    out.worst_utilisation = budgets.peak_utilisation(out.worst_domain);
    out.retirement = default_epoch_manager().stats();
    out.process_lifetime_bytes = cy::process_lifetime_bytes();
    out.process_lifetime_allocations = cy::process_lifetime_count();
    out.allocator_scope_depth = allocator_scope_depth();
    out.allocator_scope_overflows = allocator_scope_overflows();
    return out;
}

void memory_trace_report() noexcept {
    if (!diag::trace_is_open()) {
        return;
    }
    const MemoryDiagnostics snapshot = memory_diagnostics();

    for (u32 index = 0; index < kMemoryDomainCount; ++index) {
        const auto which = static_cast<MemoryDomain>(index);
        emit_counter(domain_counter_name(which), snapshot.domains[index].live_bytes);
    }
    emit_counter("memory.total_live_bytes", snapshot.total_live_bytes);
    emit_counter("memory.reserved_bytes", snapshot.total_reserved_bytes);
    emit_counter("memory.retirement_depth", snapshot.retirement.depth);
    emit_counter("memory.pressure_level", static_cast<u64>(snapshot.pressure));

    // One instant carrying the whole snapshot as classified fields, so a capture has the figures
    // together as well as as separate series.
    const diag::FieldValue fields[] = {
        diag::field_u64(live_bytes(), snapshot.total_live_bytes),
        diag::field_u64(peak_bytes(), snapshot.total_peak_bytes),
        diag::field_u64(reserved_bytes(), snapshot.total_reserved_bytes),
        diag::field_u64(live_allocations(),
                        snapshot.domains[static_cast<u32>(MemoryDomain::Engine)].live_allocations),
        text_field(pressure_level(), pressure_level_name(snapshot.pressure)),
        diag::field_u64(pressure_transitions(), snapshot.pressure_transitions),
        text_field(domain(), domain_name(snapshot.worst_domain)),
        diag::field_f64(utilisation(), snapshot.worst_utilisation),
        diag::field_u64(retirement_depth(), snapshot.retirement.depth),
        diag::field_u64(retirement_refused(), snapshot.retirement.refused),
        diag::field_u64(stalled_epochs(), default_epoch_manager().stalled_epochs()),
        diag::field_u64(held_bytes(), snapshot.process_lifetime_bytes),
    };
    static const diag::NameId name = diag::register_name("memory.report");
    diag::trace_instant(name, category(), diag::Channel::Verbose, fields,
                        static_cast<u32>(sizeof(fields) / sizeof(fields[0])));
}

void memory_trace_pressure(PressureLevel level, PressureLevel previous, MemoryDomain cause,
                           f64 level_utilisation) noexcept {
    if (!diag::trace_is_open()) {
        return;
    }
    // Important, not Verbose: a pressure transition is what a capture is read against when memory
    // ran out, and the loss policy must not drop it before the counters around it.
    const diag::FieldValue fields[] = {
        text_field(pressure_level(), pressure_level_name(level)),
        text_field(previous_level(), pressure_level_name(previous)),
        text_field(domain(), domain_name(cause)),
        diag::field_f64(utilisation(), level_utilisation),
    };
    static const diag::NameId name = diag::register_name("memory.pressure");
    diag::trace_instant(name, category(), diag::Channel::Important, fields,
                        static_cast<u32>(sizeof(fields) / sizeof(fields[0])));
}

void memory_trace_budget_violation(MemoryDomain which, u64 wanted, u64 allowance) noexcept {
    if (!diag::trace_is_open()) {
        return;
    }
    const diag::FieldValue fields[] = {
        text_field(domain(), domain_name(which)),
        diag::field_u64(wanted_bytes(), wanted),
        diag::field_u64(budget_bytes(), allowance),
        diag::field_u64(live_bytes(), domain_stats_recursive(which).live_bytes),
    };
    static const diag::NameId name = diag::register_name("memory.budget_violation");
    diag::trace_instant(name, category(), diag::Channel::Important, fields,
                        static_cast<u32>(sizeof(fields) / sizeof(fields[0])));
}

void memory_trace_eviction(MemoryDomain which, u64 bytes) noexcept {
    if (!diag::trace_is_open()) {
        return;
    }
    const diag::FieldValue fields[] = {
        text_field(domain(), domain_name(which)),
        diag::field_u64(evicted_bytes(), bytes),
        diag::field_u64(live_bytes(), domain_stats_recursive(which).live_bytes),
    };
    static const diag::NameId name = diag::register_name("memory.eviction");
    diag::trace_instant(name, category(), diag::Channel::Important, fields,
                        static_cast<u32>(sizeof(fields) / sizeof(fields[0])));
}

void memory_log_report() noexcept {
    BudgetRow rows[kMemoryDomainCount];
    const u32 count = default_budget_tree().report(rows, kMemoryDomainCount);
    for (u32 index = 0; index < count; ++index) {
        const BudgetRow& row = rows[index];
        const diag::LogLevel level =
            row.over_budget ? diag::LogLevel::Warning : diag::LogLevel::Info;
        CY_LOG(log_category(), level, "memory.budget",
               text_field(domain(), domain_name(row.domain)),
               diag::field_u64(budget_bytes(), row.budget),
               diag::field_u64(live_bytes(), row.live_bytes),
               diag::field_u64(peak_bytes(), row.peak_bytes),
               diag::field_f64(utilisation(), row.utilisation),
               diag::field_u64(evicted_bytes(), row.evicted_bytes));
    }

    const MemoryDiagnostics snapshot = memory_diagnostics();
    CY_LOG(log_category(), diag::LogLevel::Info, "memory.summary",
           diag::field_u64(live_bytes(), snapshot.total_live_bytes),
           diag::field_u64(peak_bytes(), snapshot.total_peak_bytes),
           diag::field_u64(reserved_bytes(), snapshot.total_reserved_bytes),
           text_field(pressure_level(), pressure_level_name(snapshot.pressure)),
           diag::field_u64(retirement_depth(), snapshot.retirement.depth),
           diag::field_u64(held_bytes(), snapshot.process_lifetime_bytes));
}

namespace {

/// One leaked allocation, as a log record. A sink rather than a loop inside the tracker, so that
/// `TrackingAllocator` does not have to know that diagnostics exist.
void log_one_leak(const TrackedAllocation& allocation, void* user) noexcept {
    (void)user;
    CY_LOG(log_category(), diag::LogLevel::Error, "memory.leak",
           text_field(allocation_tag(), allocation.tag),
           diag::field_u64(live_bytes(), allocation.bytes),
           text_field(allocation_site(), allocation.site.file),
           diag::field_u64(live_allocations(), allocation.site.line));
}

}  // namespace

void memory_log_leak_report(const TrackingAllocator* tracker) noexcept {
    LeakReport report;
    if (tracker != nullptr) {
        report = tracker->report_leaks(&log_one_leak, nullptr);
    }
    report.process_lifetime_bytes += cy::process_lifetime_bytes();
    report.process_lifetime_allocations += cy::process_lifetime_count();

    const diag::LogLevel level =
        (report.leaked_allocations != 0) ? diag::LogLevel::Error : diag::LogLevel::Info;
    CY_LOG(log_category(), level, "memory.leak_report",
           diag::field_u64(leaked_allocations(), report.leaked_allocations),
           diag::field_u64(leaked_bytes(), report.leaked_bytes),
           diag::field_u64(held_bytes(), report.process_lifetime_bytes),
           diag::field_u64(live_allocations(), report.live_allocations));
}

}  // namespace cy
