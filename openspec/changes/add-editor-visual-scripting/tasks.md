# Tasks

## 1. Engine

- [x] 1.1 `compile_event_graph`, `EventProgram`, `ExternalDecl`, `waiting_at` and `disassemble`; `script.on_event`; `execute_from` and `execute_native_from`.
- [x] 1.2 `cy::game_backend::GraphBehaviours`: the gameplay vocabulary and its metadata, one compile and one binding per graph, a dense instance array, the mover and the `unit.arrived` wait, cues through the ABI audio backend, both back ends.
- [x] 1.3 `script.catalogue.get`, `script.compile`, `script.event.raise`, `script.state.get` on `MaterialService` through `ScriptPlayRuntime`.
- [x] 1.4 The hosted runtime's `GraphRuntime`: attach at Play, tick after Swift, refuse Play naming the node, answer the editor's raise and state.
- [x] 1.5 `integration.graph_compiler` (event graphs), `integration.game_backend_graph`, `integration.editor_backend_script` and the committed wire.
- [x] 1.6 `smoke.editor_graph_equivalence`: the editor's graph against its Swift twin, `CommandedUnit`, tick for tick.
- [x] 1.7 `cy_game_backend` compiled with `-ffp-contract=off`: Swift never contracts, and a fused `x += dx / distance * step` moved the graph unit off its twin by an ULP on arm64 and Apple silicon; `integration.game_backend_graph` walks `step_towards` against an unfused reference.

## 2. Editor

- [x] 2.1 `cy_editor_services::script_graph`: the canonical `cygraph 1` writer and reader, `%.9g`, and the wire.
- [x] 2.2 `ScriptRequests`: the catalogue on demand, compiles kept per graph with the source they answered, Play's state polled while the panel is drawn.
- [x] 2.3 Commands with MCP parity: `script.graph.create`, `script.node.{add,move,connect,disconnect,remove,property.set}`, `script.graph.attach` (undoable); `script.graph.{read,compile}`, `script.event.raise`, `script.refresh`, `script.status` (reads).
- [x] 2.4 `Domain::GameplayAndUtilityGraphs` on the engine's catalogue; the Gameplay Graph panel on the scaffold.

## 3. Records

- [x] 3.1 Rust cases in services, interface, shell and MCP; panel snapshots `docs/design/images/editor-gameplay-graph*.png`.
- [x] 3.2 Mutation proofs in `evidence/falsification.txt`.
- [x] 3.3 `tools/roadmap/requirements-coverage.toml`: the three added requirements and their tests recorded for mapping on archive (the `visual-scripting` row maps none yet, on main or here).
- [x] 3.4 `docs/guides/visual-scripting.md`, `editor/README.md`, `src/graph/README.md`, `src/editor_backend/README.md`, `src/game_backend` header, `samples/05b-editor-window/README.md`.
- [ ] 3.5 On archive, add the `test:`/`rust:` entries recorded in the coverage file.

## 4. Later slices (not in this change)

- [ ] 4.1 The Play debugger: breakpoints, stepping, watch values and execution highlighting over the debug map.
- [ ] 4.2 Hot reload of an edited graph during Play, with the declared state migration policies.
- [ ] 4.3 Semantic diff and three-way merge of `.cyscript` in the editor's merge panel (the engine's `merge.h` already reads the files).
- [ ] 4.4 Swift interop beyond shared engine services: a graph calling Swift-exposed functions and Swift raising graph events.
- [ ] 4.5 AI behaviour graphs (`ai.*`, `lower_behaviour`) and ability pipeline graphs (`ability.*`, `compile_ability`) in the panel.
- [ ] 4.6 An explicit, declared per-tick event; graph tests against a mocked context; a determinism audit run on every compile.
