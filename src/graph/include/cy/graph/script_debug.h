// SPDX-License-Identifier: MIT
#pragma once
// The graph debugger's engine services: breakpoints and stepping at node granularity, pin and
// variable watches, over the COMPILED program and its debug map. Issue #84, stage 3; issue #29.
//
// ================================================================================================
// NOT AN INTERPRETER, AND NOT A COST ON A PROGRAM NOBODY IS DEBUGGING
// ================================================================================================
//
// `visual-scripting`: "Debugging SHALL work against compiled graphs, not only interpreted ones: the
// compiler SHALL emit a map from program locations to node and pin identity." So the debugger does
// not walk a graph. `instrument_for_debug` copies a compiled program and puts one `ScriptOp::Probe`
// before each node's instructions — the node the debug map says they came from — and nothing else
// changes: the same blocks, the same registers, the same suspend points, the same externals. An
// instance's `ScriptState` is therefore valid against both programs, and switching an instance
// between them at a tick boundary is free.
//
// A probe is the ONLY place the run loops consult a `ScriptDebugHook`. A program the compiler
// emitted has none, so a game that is not being debugged runs exactly the code it would otherwise
// run, on either back end. In a build without `CY_DEVELOPMENT` (Profile and Shipping) the probe
// test is compiled out of the bytecode loop, the native probe step is empty and
// `instrument_for_debug` refuses, so no shipped program can carry a probe either.
// `kGraphDebuggerEnabled` says which.
//
// ================================================================================================
// WHAT A PROBE KNOWS, AND WHERE A PAUSED INSTANCE CONTINUES
// ================================================================================================
//
// A `ProbeSite` names the node about to run, the block and the offset of the probe, and whether the
// node sits on an execution chain (a call, an emission, a wait, a variable write, a branch) or is a
// data node evaluated to feed one. A hook that answers `Break` leaves the instance PAUSED: the run
// returns `RunOutcome::Paused` and the state records the block and the offset just after the probe.
// Both back ends record and resume a pause in those terms, as they do a suspension, so the same
// breakpoint stops at the same node on the bytecode and the native back end.
//
// A handler's `script.on_event` node compiles to no instruction, so the instrumented program also
// probes each handler's first block with the event node: a breakpoint on the event stops before the
// handler's first node runs.

#include <cy/core/base/expected.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/event_script.h>
#include <cy/graph/lower_script.h>

namespace cy::graph::script {

/// A copy of `program` with a probe at every node boundary. Refused where `kGraphDebuggerEnabled`
/// is false. The copy borrows nothing from `program`.
[[nodiscard]] Expected<ScriptProgram, Error> instrument_for_debug(
    const ScriptProgram& program) noexcept;

/// The same for an event graph, with an entry probe on each handler's event node.
[[nodiscard]] Expected<EventProgram, Error> instrument_for_debug(
    const EventProgram& program) noexcept;

/// The value a node's pin holds in one instance.
struct PinReading {
    bool found = false;
    Value value;
    ValueKind kind = ValueKind::Void;
    /// The register it was read from.
    Reg reg = kNoRegister;
};

/// Read `node`'s `pin` in `state` through the debug map: the register the instruction recorded
/// against that node and pin writes. A data node evaluated in more than one block has a register in
/// each; `prefer` names the block the instance is in (for example the paused block), and otherwise
/// the last one is read. Pins the map records: a node's output `value`, a call's or emission's
/// packed arguments (`arg0`, `arg1`), an unwired input's zero, and a variable write's `value`.
[[nodiscard]] PinReading read_pin(const ScriptProgram& program, const ScriptState& state,
                                  NodeKey node, Name pin, BlockId prefer = kNoBlock) noexcept;

/// The variable declared by node `id`, or null.
[[nodiscard]] const Variable* find_variable(const ScriptProgram& program, NodeKey id) noexcept;

/// The variable named `name`, or null.
[[nodiscard]] const Variable* find_variable_named(const ScriptProgram& program, Name name) noexcept;

/// A variable's value in one instance.
[[nodiscard]] Value read_variable(const Variable& variable, const ScriptState& state) noexcept;

}  // namespace cy::graph::script
