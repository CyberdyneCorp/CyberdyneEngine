# Visual scripting in CyberEngine

Gameplay graphs are CyberEngine's visual scripting: a graph authored in the editor's Gameplay Graph
panel, saved as a project `.cyscript`, attached to an entity, compiled by the engine to its typed
register machine and run during Play by the engine's compiled program. The editor interprets nothing.
This guide is a route through the pieces; the contract is the
[`visual-scripting`](../../openspec/specs/visual-scripting/spec.md) specification and, until it is
archived, the change
[`add-editor-visual-scripting`](../../openspec/changes/add-editor-visual-scripting/proposal.md).

## The pieces

| Layer | Where | What it does |
|---|---|---|
| Compiler | `src/graph/` (`event_script.h`) | `compile_event_graph` lowers every `script.on_event` handler into one shared `ScriptProgram`; names are checked against the host's declared externals |
| Host | `src/game_backend/` (`graph_behaviours.h`) | `GraphBehaviours` compiles each graph once, binds every name once and runs one system over a dense array of instances; cues go through the ABI audio backend a Swift `Audio.play` reaches |
| Backend service | `src/editor_backend/` (`script_service`) | `script.catalogue.get`, `script.compile`, `script.event.raise`, `script.state.get` |
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
| `integration.graph_compiler` | Event graphs compile; every refusal is a diagnostic on its node |
| `integration.game_backend_graph` | The acceptance graph on the bytecode and native back ends, tick for tick; the unfused step |
| `integration.editor_backend_script` | The four backend-service operations against the committed wire fixtures |
| `smoke.editor_graph_equivalence` | The editor's graph against `CommandedUnit.swift` in two Play sessions (needs a Swift toolchain) |
| `cargo test` in `editor/` | The writer, the commands, the panel and the MCP tools |

## Not built yet

The Play debugger (breakpoints, stepping, watches), hot reload with state migration, semantic diff and
merge of `.cyscript` in the merge panel, Swift interop beyond shared engine services, AI behaviour and
ability graphs in the panel, and an explicit per-tick event. See the change's `tasks.md` §4 and the
[editor README](../../editor/README.md#gameplay-graphs-visual-scripting).
