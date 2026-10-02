# Visual scripting in CyberEngine

Gameplay graphs are CyberEngine's visual scripting: a graph authored in the editor's Gameplay Graph
panel, saved as a project `.cyscript`, attached to an entity, compiled by the engine to its typed
register machine and run during Play by the engine's compiled program. The editor interprets nothing.
This guide is a route through the pieces; the contract is the
[`visual-scripting`](../../openspec/specs/visual-scripting/spec.md) specification and, until it is
archived, the changes
[`add-editor-visual-scripting`](../../openspec/changes/add-editor-visual-scripting/proposal.md) and
[`add-visual-scripting-debugger`](../../openspec/changes/add-visual-scripting-debugger/proposal.md).

## The pieces

| Layer | Where | What it does |
|---|---|---|
| Compiler | `src/graph/` (`event_script.h`) | `compile_event_graph` lowers every `script.on_event` handler into one shared `ScriptProgram`; names are checked against the host's declared externals |
| Host | `src/game_backend/` (`graph_behaviours.h`) | `GraphBehaviours` compiles each graph once, binds every name once and runs one system over a dense array of instances; cues go through the ABI audio backend a Swift `Audio.play` reaches |
| Debugger and reload | `src/graph/` (`script_debug.h`, `script_reload.h`) | Probes at node boundaries in a copy of the compiled program; pin and variable reads; state migration by variable identity |
| Backend service | `src/editor_backend/` (`script_service`) | `script.catalogue.get`, `script.compile`, `script.event.raise`, `script.state.get`, `script.debug.get`, `script.debug.breakpoint`, `script.debug.control`, `script.reload` |
| Play | `samples/05b-editor-window/runtime/graph_runtime.cpp` | Attaches every `ScriptGraph` when Play starts and ticks graphs after the Swift behaviours |
| Editor | `editor/crates/cy-editor-shell/src/panels/script_graph.rs` | The panel; every edit is an undoable command and an MCP tool of the same name |

## Authoring a graph

Open the Gameplay Graph panel. Its palette is the engine's: a property that names an engine function,
query or wait is a choice among the names the engine declares
(`cy::game_backend::gameplay_graph_externals()`), so a misspelled name cannot be typed. A graph answers
events (`On Event`, for example `unit.command`); there is no per-frame entry, and a graph that keeps
working across frames waits (`unit.arrived`) and is resumed once.

The same edits are MCP tools: `script.graph.create`, `script.node.add`, `script.node.move`,
`script.node.connect`, `script.node.disconnect`, `script.node.remove`, `script.node.property.set` and
`script.graph.attach` (each one undo step), plus the reads `script.graph.read`, `script.graph.compile`,
`script.status`, `script.refresh` and `script.event.raise`. The file is CyberGraph's canonical
`cygraph 1` text, byte for byte what `cy::graph::write_graph` writes, so a text diff is a semantic diff.

The engine compiles every change. A diagnostic outlines its node on the canvas and is listed under it
with node, pin, code and message: an undeclared name (`script.external.unknown`), a name used as the
wrong kind, a missing capability, a missing or duplicated event, an unknown node type, or a wire between
different pin types. A node no event reaches is a warning.

## Running it

`script.graph.attach` gives an entity a `ScriptGraph` component whose `graph` field names the file.
Entering Play compiles each graph once and refuses Play, naming the node, when a graph does not compile
or plays a cue the project lacks. The panel's Play row raises an event on the selected entity and shows
each instance (waiting on what, where, how many runs) and every cue played, with its tick.

## Variables

A `Variable` node (`script.variable`) declares state each entity running the graph keeps across events
and waits: a `name`, a `type` (`float`, `int` or `bool`) and a `default`. `Get Variable` and
`Set Variable` read and write it. A variable's identity is the node that declares it, not its name:
renaming it keeps its value across a reload. An undeclared, unnamed, duplicated or mistyped variable is
an error on its node. `src/editor_backend/tests/data/script_unit_counter_v1.cyscript` is the unit graph
with an `orders` count.

## Debugging in Play

In a Debug or Development build, Play runs every graph's debug-instrumented program: the compiled
program with a probe before each node's instructions, and nothing else changed. A program nobody
debugs has no probe, and Profile and Shipping builds have no debugger at all (`kGraphDebuggerEnabled`
follows `CY_DEVELOPMENT`; instrumentation refuses there and the run loops contain no probe test).

- **Breakpoints.** Click the dot at the right of a node's header. With *Break only for <entity>*
  checked, the breakpoint stops only the selected entity's instance. Breakpoints set before Play take
  effect when Play starts; one on an event node stops before the handler's first node.
- **What pauses.** A break pauses the whole simulation tick, as a Blueprint breakpoint stops the game
  thread: the Play session, its physics, the Swift behaviours, the clock and Play's audio wait, and no
  event can be raised. The rest of the tick's graph work is held in order and runs on when you continue
  or step, before any later tick. A run that breaks and steps is therefore the same run, placement for
  placement and cue for cue, as one that does not; resuming Play from the toolbar continues the graph.
- **Stepping.** *Step Over* runs the paused entity's graph to its next node on the execution chain;
  *Step Into* also stops at each data node feeding it. A step across a wait stays armed and stops when
  the entity resumes. *Continue* runs to the next breakpoint; *Pause* stops at the next node any graph
  runs.
- **Watches.** The watch list shows the inspected entity's variables (the paused one, or the selected
  entity after *Inspect*) and the pins you watch (*Watch node N*, a node's `value`; over MCP also a
  call's `arg0` / `arg1`). Values are read from the running program through its debug map.
- **Execution highlighting.** The last six nodes the engine's trace says ran glow on the canvas, the
  newest brightest; the paused node is outlined.

Over MCP: `script.debug.breakpoint`, `script.debug.pause`, `script.debug.continue`, `script.debug.step`
(`mode` `over` or `into`), `script.debug.watch`, `script.debug.inspect`, `script.debug.refresh` and
`script.debug.status`, which reports the paused node, the trace, the variables and the watches.

## Editing while Play runs

Saving a graph Play runs — an edit in the panel, an MCP tool, an undo or a redo — reloads it there
(`script.graph.reload` does it by hand). The engine recompiles it and swaps the new program in for every
entity running it at the next tick boundary, never in the middle of a tick and never while a graph is
paused. Each entity's variables move by identity: kept at their value, new ones at their default,
removed ones dropped; a wait in progress continues when its wait node is still there. A variable whose
type changed refuses the reload with `script.reload.type` on its node, and the previous program keeps
running; so does a graph that no longer compiles. The panel shows what the reload kept, or the refusal
on its node.

## Agreeing with Swift, float for float

The acceptance graph `src/editor_backend/tests/data/script_unit_command_v1.cyscript` has a Swift twin,
`samples/05b-editor-window/project/game/CommandedUnit.swift`, and `smoke.editor_graph_equivalence`
requires the two to produce the same position on every tick. Movement is stated once, in
`cy::game_backend::step_towards`, and the Swift twin writes the same expression.

Swift never contracts floating-point arithmetic. C++ compilers do by default where the target has a
fused multiply-add (GCC on aarch64, clang on Apple silicon), which turns `x += dx / distance * step`
into one rounding instead of two. `cy_game_backend` is therefore compiled with `-ffp-contract=off`
(`/fp:precise` on MSVC); without it the graph unit leaves its twin by an ULP from the twentieth tick on
arm64. `integration.game_backend_graph` holds this on any host: it walks `step_towards` against a
reference whose every product is rounded before the add.

## The suites

| Suite | What it holds |
|---|---|
| `integration.graph_compiler` | Event graphs compile; every refusal is a diagnostic on its node; probes, pauses on both back ends, variables, migration |
| `integration.game_backend_graph` | The acceptance graph on the bytecode and native back ends, tick for tick; the unfused step |
| `integration.editor_backend_script` | The backend-service operations, debugger and reload included, against the committed wire fixtures |
| `integration.game_backend_graph` (debugger cases) | Breakpoints per entity, the held tick, a debugged run equal to an undebugged one, step order, watches, reloads |
| `integration.editor_window_graph_debugger` | A graph break pauses the whole `PlaySession`; a reload through the hosted runtime keeps a count |
| `smoke.editor_graph_equivalence` | The editor's graph against `CommandedUnit.swift` in two Play sessions (needs a Swift toolchain) |
| `cargo test` in `editor/` | The writer, the commands, the panel and the MCP tools |

## Not built yet

Per-node profiling and a cost heat map, migration policies other than keep-by-identity, predicted and
authoritative execution side by side, semantic diff and merge of `.cyscript` in the merge panel, Swift
interop beyond shared engine services, AI behaviour and ability graphs in the panel, and an explicit
per-tick event. See the changes' `tasks.md` and the
[editor README](../../editor/README.md#gameplay-graphs-visual-scripting).
