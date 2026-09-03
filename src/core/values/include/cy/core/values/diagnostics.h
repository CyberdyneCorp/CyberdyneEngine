#pragma once
// What the value layer reports about itself, on the M0 trace. Task 1.3.6.
//
// `core-type-system` — "Diagnostics" — makes `core/diagnostics` the engine's error vocabulary, and
// `diagnostics-profiling-and-crash` makes it one trace with many producers. So this module does not
// define a second timeline, a second buffer or a second artefact: it defines what it emits onto the
// one that exists, and every field it emits carries a privacy classification because the field
// macro has no overload that omits one.
//
// COUNTERS, NOT EVENTS, ON THE PATHS THAT MATTER. Interning a name, copying a `Var` and — above all
// — validating a handle are called at a rate that makes per-occurrence emission a cost rather than
// a measurement. Each of those bumps a relaxed atomic. `values_trace_report()` is the control-plane
// half: called at a frame boundary, a level transition or a shutdown, it emits the counters as one
// record apiece. The question it answers is "what is this process doing to the value layer" —
// whether the intern table is growing without bound, whether copy-on-write is detaching in a loop,
// and whether stale handles are being rejected, which is a symptom of an ownership bug upstream.
//
// This header deliberately does not include anything from `core/diagnostics`. The dependency is an
// implementation detail of diagnostics.cpp, so a consumer of `Var` does not acquire the trace's
// headers by including a value type.

#include <cy/core/base/types.h>
#include <cy/core/values/name.h>

namespace cy::values {

/// A snapshot of every counter the value layer keeps. Cheap: a read of each atomic.
struct ValueDiagnostics {
    // Interning.
    u32 names_interned = 0;  ///< entries in the table, including the empty name
    u64 name_bytes = 0;      ///< text storage in use
    u64 name_lookups = 0;
    u64 name_insertions = 0;
    u64 name_rejections = 0;  ///< text longer than Name::kMaxLength

    // Var's heap.
    u64 var_blocks_allocated = 0;
    u64 var_blocks_freed = 0;
    u64 var_blocks_live = 0;
    u64 var_blocks_detached = 0;  ///< copy-on-write clones

    // Handles.
    u64 handle_slots_allocated = 0;
    u64 handle_slots_freed = 0;
    u64 handle_slots_live = 0;
    u64 handle_chunks_committed = 0;
    /// Handles rejected by the generation comparison. Not an error in itself — asking whether a
    /// handle is still live is what the type is for — but a rate that climbs is an owner freeing
    /// something another system still refers to.
    u64 stale_handle_rejections = 0;

    // Signals and callables.
    u64 signal_emissions = 0;
    u64 signal_invocations = 0;
    u64 signal_deferred = 0;
    u64 signal_connections_pruned = 0;
    u64 call_invocations = 0;
    u64 call_failures = 0;
};

[[nodiscard]] ValueDiagnostics values_diagnostics() noexcept;

/// Emit the snapshot to the trace as counter records on the `values` category. Control plane: call
/// it at a frame boundary or a shutdown, never inside a loop over values.
///
/// Does nothing when no trace is open, which is the ordinary case in a shipping process.
void values_trace_report() noexcept;

/// Log the snapshot at Info on the `values` category, as one record with the counters as fields.
/// This is what a `just` recipe or a headless sample calls to leave the numbers in the log.
void values_log_report() noexcept;

/// Report a call that failed, with the callable's name as a classified field.
///
/// The name is a project identifier — a script function, a method — so it is `Developer`, not
/// `Public`: it is engine-internal detail that belongs in a developer's own capture and not in an
/// artefact prepared to leave the machine. It is a *field*, never the event name, for the reason
/// design.md's diagnostics correction gives: a name is structurally beyond the writer's redaction,
/// and a classified field is not.
void values_log_call_failure(Name callable_name, const char* error_kind) noexcept;

}  // namespace cy::values
