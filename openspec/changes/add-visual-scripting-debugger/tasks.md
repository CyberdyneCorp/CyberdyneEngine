# Tasks

## 1. Engine

- [x] 1.1 Graph variables: `script.variable`, `script.get_var`, `script.set_var`; `ScriptProgram::variables()`; diagnostics `script.variable.{unnamed,type,duplicate,unknown}`.
- [x] 1.2 `ScriptOp::Probe`, `ProbeSite`, `RunOutcome::Paused`, `ScriptState`'s pause point, `ScriptDebugHook` on `execute*` (both back ends); `kGraphDebuggerEnabled` follows `CY_DEVELOPMENT`.
- [x] 1.3 `instrument_for_debug`, `read_pin`, `find_variable`, `read_variable` (`script_debug.h`); pins recorded in the debug map.
- [x] 1.4 `check_migration`, `migrate_state` (`script_reload.h`).
- [x] 1.5 `GraphBehaviours`: debugging on/off, breakpoints per graph, node and entity, pause, step into and over, continue, the held tick, the trace, pin and variable watches, staged reloads at the tick boundary.
- [x] 1.6 `script.debug.get`, `script.debug.breakpoint`, `script.debug.control`, `script.reload` on `MaterialService`; `ScriptPlayRuntime`'s three defaulted methods.
- [x] 1.7 Hosted runtime: debugging attached in development Play, the `PlaySession` and audio held while a graph is, Play's resume continues a held graph, `reload` by reference.

## 2. Editor

- [x] 2.1 `cy_editor_services::script_debug`: requests and the debug and reload replies.
- [x] 2.2 `ScriptRequests`: wanted breakpoints sent when Play starts, the watch list, debug polling, controls and reloads.
- [x] 2.3 Commands with MCP parity: `script.debug.{breakpoint,pause,continue,step,watch,inspect,refresh,status}`, `script.graph.reload`; reload on save, undo and redo of a graph Play runs.
- [x] 2.4 The panel: gutter, paused outline, recent-execution glow, controls, watch list, reload result.

## 3. Records

- [x] 3.1 Suites: `integration.graph_compiler` (debugger, variables, reload), `integration.game_backend_graph`, `integration.editor_backend_script` and the committed wire, `integration.editor_window_graph_debugger`, Rust cases in services, shell and MCP.
- [x] 3.2 Mutation proofs in `evidence/falsification.txt`.
- [x] 3.3 Panel snapshot `docs/design/images/editor-gameplay-graph-debugger.png`.
- [x] 3.4 `docs/guides/visual-scripting.md`, `src/graph/README.md`, `editor/README.md`, `tools/roadmap/requirements-coverage.toml`.
- [ ] 3.5 On archive, map the requirements recorded in the coverage file.

## 4. Later

- [ ] 4.1 Per-node profiling and a heat map of cost.
- [ ] 4.2 Declared migration policies beyond keep-by-identity: reset, restart.
- [ ] 4.3 Predicted and authoritative execution side by side.
