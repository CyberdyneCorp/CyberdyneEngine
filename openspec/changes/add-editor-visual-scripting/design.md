# Design: gameplay graphs in the editor (#29, Wave 2, first slice)

## Context

CyberGraph compiles; it does not interpret. `compile_script` lowers one entry chain to a typed register
machine and `compile_ability` lowers an ordered stage table onto the same builder; `execute` and
`execute_native` run one instance's `ScriptState` against a shared, immutable program and a `ScriptHost`.
What did not exist: an event-shaped gameplay graph, a host that runs graphs on scene entities during
Play, a wire between the editor and either, and a panel.

The hosted runtime (`samples/05b-editor-window/runtime`) runs Swift behaviours during Play through
`ScriptRuntime` and the C ABI, and answers the editor's backend-service requests through
`cy::editor::MaterialService`. Wave 0 (#59) gave specialised tools one scaffold and one graph canvas.

## Decisions

### An event graph is one program with a handler table

`visual-scripting` makes events, not a tick, the default. An event graph's entry points are
`script.on_event` nodes, each naming the event it answers. `compile_event_graph` hands every handler's
first node to the same `ProgramBuilder` (through `script_build.h`), so the program is one `ScriptProgram`
and `EventProgram` records which block each event begins at. `execute_from` / `execute_native_from` start
an instance at a handler's block and discard a suspension it was in: a newer order replaces the one in
progress, which is what an RTS command means. A wait is resumed by the scheduler: `waiting_at` finds the
suspend point by the instance's resume block, the host answers its reason, and `execute` resumes it.

There is deliberately no tick node. A unit ordered to move waits on `unit.arrived` and is resumed once,
rather than asking every frame. An explicit declared tick is a later slice.

### Names are checked against the host's declarations, on the node

`ExternalDecl` is function metadata: a name (or a family prefix such as `cue.`), its kind (call, query,
event, command, field, wait), its arity, its capability and a sentence. The compiler refuses a name the
host did not declare, a name used as another kind, and a name whose capability the graph was not granted,
each as a `Diagnostic` on the node that names it with the name in `detail`. It also refuses a graph that
answers no event, an unnamed or duplicated event, a node type the registry lacks, and a wire between pins
of different types ("expected float, received exec"). It warns about a node no event reaches. Every
problem is reported before the program is refused, so one compile names them all.

### The host binds every name once and runs one system

`cy::game_backend::GraphBehaviours` lives beside the ABI 1.3 adapters, because it plays cues through one
of them. Loading a graph parses the `.cyscript`, compiles it, optionally compiles the native program, and
binds each external and each suspend point to a verb, resolving a `cue.<name>` to the adapter's cue
handle. At run time an external is an index; nothing is looked up by name. `update()` is the one system:
it steps every unit with an order, then resumes every satisfied wait, over a dense array of instances,
each a `ScriptState`. A graph is compiled once however many entities run it.

Movement is `step_towards`, stated once: towards the target in the ground plane, `speed * dt`, arriving
exactly on it. A Swift behaviour that does the same thing must use the same expression to agree float for
float, and `CommandedUnit.swift` does. The same expression is not enough on its own: Swift never contracts
floating-point arithmetic, and GCC on aarch64 and clang on Apple silicon fuse `x += dx / distance * step`
into one multiply-add by default, so `cy_game_backend` is compiled with `-ffp-contract=off`.

### The editor writes the engine's canonical text

The `.cyscript` file is `cy::graph::write_graph`'s output, so a text diff is a semantic diff and the
engine's three-way merge reads it. `cy_editor_services::script_graph` writes nodes by key, properties by
name, wires by `(to, to pin, from, from pin)`, layout last, and every float as `%.9g`. An empty name is
written as the zero tuple, as the engine writes it. A node the editor cannot read is kept verbatim.
`src/editor_backend/tests/data/script_unit_command_v1.cyscript` pins this from both sides.

### Edits go through the shared canvas with the engine's catalogue

The catalogue is the material catalogue's schema 3, so one decoder reads it. Its node types are the
engine's script vocabulary without `script.entry`. A property that names an external is an enumeration
whose choices are the declared names; a property's `semantic` is `literal:<type>`, which says how the
value is written in the text. Every edit command reads the file, lays it on a `GraphCanvas` holding that
catalogue, changes it through the canvas's checked operations, captures it and saves it as one `Domain`
operation (`script_graph:<path>`) in the active world's history, as #17's VFX and #62's audio assets do.
The panel's gestures push the same commands, so a gesture and an MCP tool are refused for the same
reasons and undo the same way.

### Compiles are reads, and the panel asks once per change

A compile changes no document, so `script.graph.compile` is a read, like `play.enter`. The editor keeps
each graph's last report with the source it answered; the panel compiles whenever the saved text differs
from that source and nothing is in flight, once per change. Diagnostics with a node outline it on the
canvas (`CanvasFeedback::node_alerts`) and list under it; a row selects the node. The canvas leaves out
its own unwired-input warnings for this domain, because an unwired gameplay argument reads zero.

### Play runs the engine's program; the editor raises events

`GraphRuntime` attaches every live node with a `ScriptGraph` component when Play starts, after the audio
has named the project's cues, and ticks the graph system after the Swift behaviours. `script.event.raise`
names the authored node by the identity the editor's mirror sends; the runtime maps it to the Play
entity. `script.state.get` reports each instance (status, wait, position, runs, problem) and every cue
played, with its tick.

### Equivalence is measured, not argued

Two suites hold the acceptance scenario. `integration.game_backend_graph` runs the editor's graph on the
bytecode and the native back end side by side and requires the same position every tick and the same cue
on the same tick. `smoke.editor_graph_equivalence` runs the editor's graph and `CommandedUnit.swift` in
two Play sessions through the hosted runtime's own glue and requires the same positions, the same arrival
tick and the same cue at the same place. It is declared wherever a Swift toolchain is.

## Not in this slice

The Play debugger (breakpoints, stepping, watches), hot reload with state migration, semantic diff and
merge in the editor's merge panel, Swift interop beyond shared engine services, AI and ability graphs in
the panel, and an explicit per-tick event. `tasks.md` §4 lists them.
