# Proposal: Gameplay graphs in the editor (visual scripting, first slice)

## Why

Issue #29 Wave 2 lists visual scripting as the largest Medium item. The engine side is built: CyberGraph
(`src/graph/`) compiles gameplay graphs to a typed register machine with a shared program and per-instance
state, on a bytecode and a native back end, and `visual-scripting` is Working. But no panel opens
`Domain::GameplayAndUtilityGraphs`, nothing saves a gameplay graph, nothing attaches one to an entity,
and nothing runs one in Play. Game logic can only be written in Swift.

## What Changes

- **Event graphs in the engine.** `cy::graph::script::compile_event_graph` compiles a graph whose entry
  points are `script.on_event` nodes into one shared `ScriptProgram` with a handler table, through the
  builder `compile_script` and `compile_ability` already use. Names are checked against the host's declared
  externals (function metadata): an undeclared name, a name used as the wrong kind, or one outside the
  graph's capabilities is a diagnostic on the node; so are a missing, unnamed or duplicate event, an
  unknown node type and a wire between pins of different types. A node no event reaches is a warning.
  `execute_from` and `execute_native_from` start a handler on either back end, and `disassemble` lists
  what a graph became.
- **Graphs on entities, on the same services Swift uses.** `cy::game_backend::GraphBehaviours` compiles
  each `.cyscript` once, binds every name once, and runs one shared program over a dense array of
  instances. It declares the gameplay vocabulary (`unit.move_to`, `unit.set_speed`, `unit.stop`,
  `event.x/y/z`, `unit.x/z`, the `unit.arrived` wait and the `cue.<name>` family) and plays cues through
  `abi::game::AudioBackend`, the adapter a Swift `Audio.play(_:at:)` reaches.
- **The backend service.** `script.catalogue.get` (the vocabulary in the material catalogue's schema 3,
  with property choices that are the declared externals), `script.compile` (the program or the
  diagnostics, and the listing), `script.event.raise` and `script.state.get` (Play's instances and
  cues) on `MaterialService`, through a host seam `cy::editor::ScriptPlayRuntime`.
- **Play.** The hosted runtime's `GraphRuntime` attaches every authored `ScriptGraph` when Play starts,
  ticks the graph system after the Swift behaviours, and refuses Play naming the node when a graph does
  not compile or plays a cue the project lacks.
- **The editor.** A Gameplay Graph panel on the specialised scaffold and the shared canvas, with the
  engine's palette (events and responses; no per-frame entry); `script.graph.create`, `script.node.add`,
  `.move`, `.connect`, `.disconnect`, `.remove`, `.property.set` and `script.graph.attach`, each one
  undoable transaction and an MCP tool of the same name; `script.graph.compile`, `script.event.raise`,
  `script.refresh` and `script.status`. The engine compiles each change; its diagnostics outline the
  node and list under the canvas, and a row selects its node. The `.cyscript` file is the engine's
  canonical CyberGraph text, byte for byte.

## Capabilities

### Modified Capabilities

- `editor-architecture`: the gameplay and utility graphs editor.
- `visual-scripting`: event graphs and host-declared function metadata.

## Impact

- Engine: `src/graph/` (`event_script.h/.cpp`; `execute_from`, `execute_native_from`; `script.on_event`
  registered with the script vocabulary), `src/game_backend/` (`graph_behaviours`, now linking
  `cy::graph`), `src/editor_backend/` (`script_service`; `MaterialService::set_scripts`; the `script.`
  prefix; `capabilities.get` lists the four operations), `samples/05b-editor-window/runtime/`
  (`graph_runtime`; Play ticks graphs; `ScriptRuntime::bind_audio` takes the ABI audio backend).
- Editor: `cy-editor-services` (`script_graph`, `script_requests`, `script_commands`; `ProjectHost`
  gains six defaulted methods), `cy-editor-interface` (`specialised::script`,
  `script_authoring_commands`, `install_script_catalogue`; `script.on_event` in both script palettes),
  `cy-editor-shell` (`panels/script_graph.rs`; the shared canvas can leave out unwired-input warnings).
- No ABI or bridge message change.
