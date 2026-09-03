// The value layer's diagnostics, emitted onto the M0 trace. Task 1.3.6.
//
// One category, `values`, and a field per counter. Every field is declared with CY_TRACE_FIELD,
// which takes its privacy classification as a required third argument — there is no overload
// without one — so the classification below is not a convention that a later edit can drop.
//
// Everything here is Public except the callable name: a count is a number about the engine, and
// nothing about a count identifies a person or a machine. `callable_name` is Developer because it
// is a project's own identifier — the name of a script function or a method — which belongs in a
// developer's own capture and not in an artefact prepared to leave the machine.

#include <cy/core/values/diagnostics.h>

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>
#include <cy/core/values/name.h>

#include "counters.h"

#include <cstring>

namespace cy::values {
namespace {

CY_TRACE_CATEGORY(category, "values")

CY_TRACE_FIELD(names_interned, u64, cy::Privacy::Public)
CY_TRACE_FIELD(name_bytes, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(name_lookups, u64, cy::Privacy::Public)
CY_TRACE_FIELD(name_insertions, u64, cy::Privacy::Public)
CY_TRACE_FIELD(name_rejections, u64, cy::Privacy::Public)
CY_TRACE_FIELD(var_blocks_live, u64, cy::Privacy::Public)
CY_TRACE_FIELD(var_blocks_allocated, u64, cy::Privacy::Public)
CY_TRACE_FIELD(var_blocks_detached, u64, cy::Privacy::Public)
CY_TRACE_FIELD(handle_slots_live, u64, cy::Privacy::Public)
CY_TRACE_FIELD(handle_chunks_committed, u64, cy::Privacy::Public)
CY_TRACE_FIELD(stale_handle_rejections, u64, cy::Privacy::Public)
CY_TRACE_FIELD(signal_emissions, u64, cy::Privacy::Public)
CY_TRACE_FIELD(signal_invocations, u64, cy::Privacy::Public)
CY_TRACE_FIELD(signal_deferred, u64, cy::Privacy::Public)
CY_TRACE_FIELD(call_invocations, u64, cy::Privacy::Public)
CY_TRACE_FIELD(call_failures, u64, cy::Privacy::Public)

// A project's own identifier, not a number about the engine. See the file comment.
CY_TRACE_FIELD(callable_name, string, cy::Privacy::Developer)
CY_TRACE_FIELD(call_error_kind, string, cy::Privacy::Public)

/// Emit one counter as a trace Counter record. Verbose, because a counter snapshot is background
/// detail: under buffer pressure it is exactly what should be dropped before a tick boundary is.
void emit_counter(const char* name, u64 value) noexcept {
    const diag::NameId id = diag::register_name(name);
    diag::trace_counter(id, category(), diag::Channel::Verbose, value);
}

}  // namespace

ValueDiagnostics values_diagnostics() noexcept {
    const detail::Counters& counters = detail::counters();
    const NameTableStats names = name_table_stats();

    ValueDiagnostics out;
    out.names_interned = names.entries;
    out.name_bytes = names.bytes;
    out.name_lookups = names.lookups;
    out.name_insertions = names.insertions;
    out.name_rejections = names.rejections;

    out.var_blocks_allocated = counters.var_blocks_allocated.load(std::memory_order_relaxed);
    out.var_blocks_freed = counters.var_blocks_freed.load(std::memory_order_relaxed);
    out.var_blocks_live = out.var_blocks_allocated - out.var_blocks_freed;
    out.var_blocks_detached = counters.var_blocks_detached.load(std::memory_order_relaxed);

    out.handle_slots_allocated = counters.handle_slots_allocated.load(std::memory_order_relaxed);
    out.handle_slots_freed = counters.handle_slots_freed.load(std::memory_order_relaxed);
    out.handle_slots_live = out.handle_slots_allocated - out.handle_slots_freed;
    out.handle_chunks_committed = counters.handle_chunks_committed.load(std::memory_order_relaxed);
    out.stale_handle_rejections = counters.stale_handle_rejections.load(std::memory_order_relaxed);

    out.signal_emissions = counters.signal_emissions.load(std::memory_order_relaxed);
    out.signal_invocations = counters.signal_invocations.load(std::memory_order_relaxed);
    out.signal_deferred = counters.signal_deferred.load(std::memory_order_relaxed);
    out.signal_connections_pruned =
        counters.signal_connections_pruned.load(std::memory_order_relaxed);
    out.call_invocations = counters.call_invocations.load(std::memory_order_relaxed);
    out.call_failures = counters.call_failures.load(std::memory_order_relaxed);
    return out;
}

void values_trace_report() noexcept {
    if (!diag::trace_is_open()) {
        return;
    }
    const ValueDiagnostics snapshot = values_diagnostics();

    emit_counter("values.names_interned", snapshot.names_interned);
    emit_counter("values.name_bytes", snapshot.name_bytes);
    emit_counter("values.var_blocks_live", snapshot.var_blocks_live);
    emit_counter("values.var_blocks_detached", snapshot.var_blocks_detached);
    emit_counter("values.handle_slots_live", snapshot.handle_slots_live);
    emit_counter("values.stale_handle_rejections", snapshot.stale_handle_rejections);
    emit_counter("values.signal_invocations", snapshot.signal_invocations);
    emit_counter("values.call_failures", snapshot.call_failures);

    // One instant carrying the whole snapshot as classified fields, so a capture has the figures
    // together as well as as separate series.
    const diag::FieldValue fields[] = {
        diag::field_u64(names_interned(), snapshot.names_interned),
        diag::field_u64(name_bytes(), snapshot.name_bytes),
        diag::field_u64(name_lookups(), snapshot.name_lookups),
        diag::field_u64(name_insertions(), snapshot.name_insertions),
        diag::field_u64(name_rejections(), snapshot.name_rejections),
        diag::field_u64(var_blocks_live(), snapshot.var_blocks_live),
        diag::field_u64(var_blocks_allocated(), snapshot.var_blocks_allocated),
        diag::field_u64(var_blocks_detached(), snapshot.var_blocks_detached),
        diag::field_u64(handle_slots_live(), snapshot.handle_slots_live),
        diag::field_u64(handle_chunks_committed(), snapshot.handle_chunks_committed),
        diag::field_u64(stale_handle_rejections(), snapshot.stale_handle_rejections),
        diag::field_u64(signal_emissions(), snapshot.signal_emissions),
        diag::field_u64(signal_invocations(), snapshot.signal_invocations),
        diag::field_u64(signal_deferred(), snapshot.signal_deferred),
        diag::field_u64(call_invocations(), snapshot.call_invocations),
        diag::field_u64(call_failures(), snapshot.call_failures),
    };
    static_assert(sizeof(fields) / sizeof(fields[0]) <= diag::kMaxFieldsPerRecord,
                  "the snapshot must fit one record; split it rather than dropping fields");

    static const diag::NameId report_name = diag::register_name("values.report");
    diag::trace_instant(report_name, category(), diag::Channel::Verbose, fields,
                        static_cast<diag::u32>(sizeof(fields) / sizeof(fields[0])));
}

void values_log_report() noexcept {
    const ValueDiagnostics snapshot = values_diagnostics();
    CY_LOG(category(), diag::LogLevel::Info, "values.report",
           diag::field_u64(names_interned(), snapshot.names_interned),
           diag::field_u64(name_bytes(), snapshot.name_bytes),
           diag::field_u64(var_blocks_live(), snapshot.var_blocks_live),
           diag::field_u64(var_blocks_detached(), snapshot.var_blocks_detached),
           diag::field_u64(handle_slots_live(), snapshot.handle_slots_live),
           diag::field_u64(stale_handle_rejections(), snapshot.stale_handle_rejections),
           diag::field_u64(signal_invocations(), snapshot.signal_invocations),
           diag::field_u64(call_failures(), snapshot.call_failures));
}

void values_log_call_failure(Name name, const char* error_kind) noexcept {
    const std::string_view text = name.text();
    const char* kind = error_kind != nullptr ? error_kind : "";
    CY_LOG(category(), diag::LogLevel::Warning, "values.call_failed",
           diag::field_text(callable_name(), text.data(), static_cast<diag::u32>(text.size())),
           diag::field_text(call_error_kind(), kind, static_cast<diag::u32>(std::strlen(kind))));
}

}  // namespace cy::values
