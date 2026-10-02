// SPDX-License-Identifier: MIT
#pragma once
// Hot reload of a running graph: moving an instance's state from one compiled program to the next.
// Issue #84, stage 2; issue #29.
//
// `visual-scripting`, "Hot reload and state migration": "Editing a graph in development SHALL
// recompile it and publish a new program generation, with existing instances migrated according to
// a declared policy [...] an unmigratable change SHALL be reported rather than silently resetting
// state."
//
// THE POLICY, STATED ONCE. A program's per-instance state that outlives a handler is its graph
// variables (`ScriptProgram::variables()`), and each has a STABLE IDENTITY: the key of the node
// that declares it. Moving an instance from program `from` to program `to`:
//
//   kept      a variable both declare, by identity, at the same kind: its value is carried over,
//             whatever it was renamed to
//   added     a variable only `to` declares: its declared default
//   dropped   a variable only `from` declared: gone
//   refused   a variable both declare at DIFFERENT kinds. `check_migration` reports it on the
//             declaring node (`script.reload.type`) and the reload does not happen: no instance
//             moves, and the old program keeps running
//
// A WAIT IN PROGRESS. An instance suspended on a `script.wait` keeps waiting when `to` has a wait
// node of the same key and `to` carries no register across a wait other than its variables — the
// resumed chain then needs nothing the old program computed. Otherwise the wait is dropped (the
// instance is idle and answers its next event) and `StateMigration::wait_dropped` says so. An
// instance a debugger has PAUSED mid-handler is not migrated at all: there is no node-for-node map
// between the middle of two programs, so the host reloads only at a tick boundary where nothing is
// paused.

#include <cy/core/base/expected.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_script.h>

namespace cy::graph::script {

/// What moving one instance did.
struct StateMigration {
    u32 kept = 0;
    u32 added = 0;
    u32 dropped = 0;
    /// The instance was waiting and still is, at the same wait node in the new program.
    bool wait_kept = false;
    /// The instance was waiting and the new program cannot resume it there; it is idle now.
    bool wait_dropped = false;
};

/// Whether instances of `from` can move to `to`. Every variable whose kind changed is reported on
/// its declaring node as `script.reload.type`, with `detail` "<old kind> -> <new kind>". Answers
/// the number of refusals; zero means `migrate_state` will move every instance.
[[nodiscard]] u32 check_migration(const ScriptProgram& from, const ScriptProgram& to,
                                  DiagnosticSink& sink) noexcept;

/// Move one instance's state from `from` to `to` into `target`, which must be sized for `to` (a
/// fresh `ScriptState(allocator, to)` already holds every variable's default). Refused for a paused
/// instance and for a pair `check_migration` refuses.
[[nodiscard]] Expected<StateMigration, Error> migrate_state(const ScriptProgram& from,
                                                            const ScriptState& source,
                                                            const ScriptProgram& to,
                                                            ScriptState& target) noexcept;

}  // namespace cy::graph::script
