# Proposal: The Play debugger and hot reload for gameplay graphs (visual scripting, second slice)

## Why

The first slice (`add-editor-visual-scripting`, #98) lets a person author a gameplay graph, compile it
in the engine and run it in Play. Its `tasks.md` §4.1 and §4.2, and issue #84's stages 2 and 3, are what
a graph author reaches for next: stopping a running graph at a node to see why it did what it did, and
editing a graph while the game runs without losing the state the game built up. `visual-scripting`
requires both ("Graph debugging", "Hot reload and state migration") and neither existed: no breakpoint,
no stepping, no watch, no execution history, no program generation published to running instances,
and nothing a graph could keep across events for a reload to keep.

## What Changes

- **Graph variables.** `script.variable` declares per-instance state (`name`, `type` float/int/bool,
  `default`); `script.get_var` and `script.set_var` read and write it. A variable lives in a register
  the compiler reserves for it, survives handlers and waits, and its identity is the declaring node's
  key, so a rename keeps its value across a reload. Misuse is a diagnostic on the node.
- **Debugger services in the engine** (`cy/graph/script_debug.h`). `instrument_for_debug` copies a
  compiled program with a `Probe` before each node's instructions; nothing else changes, so instance
  state is valid against both. A `ScriptDebugHook` decides at each probe; a break leaves the instance
  `Paused` at a block and offset both back ends resume from. `read_pin` reads a node's pin through the
  debug map, which now records pins. A program nobody debugs has no probe; in Profile and Shipping the
  probe test is compiled out of the loops and instrumentation refuses.
- **Hot reload** (`cy/graph/script_reload.h`). `check_migration` refuses a variable whose type changed,
  on its node; `migrate_state` keeps variables by identity, adds new ones at their default, drops
  removed ones, and keeps a wait in progress when the wait node survives and nothing else is live
  across it.
- **Play.** `GraphBehaviours` runs instrumented programs when debugging, holds breakpoints (per graph,
  node and optionally entity), pause, step into and over, a bounded trace, pin and variable watches,
  and staged reloads applied at the next tick boundary. A break holds the rest of the tick's graph work
  in order and refuses `update` and `raise` until it is continued. The hosted runtime pauses its
  `PlaySession` and Play's audio while a graph is held: the whole simulation tick pauses.
- **Backend service.** `script.debug.get`, `script.debug.breakpoint`, `script.debug.control` and
  `script.reload` on `MaterialService`, through three defaulted methods of `ScriptPlayRuntime`.
- **Editor.** A breakpoint gutter on each node, the paused node outlined, the last nodes run glowing,
  Pause / Continue / Step Over / Step Into in the panel's header, and a watch list with the inspected
  entity's variables. `script.debug.breakpoint`, `.pause`, `.continue`, `.step`, `.watch`, `.inspect`,
  `.refresh`, `.status` and `script.graph.reload` are commands and MCP tools of the same names.
  Breakpoints set before Play are sent when Play starts. Saving a graph Play runs — or undoing or
  redoing an edit to it — reloads it in Play.

## Capabilities

### Modified Capabilities

- `visual-scripting`: debugging compiled graphs, pausing the simulation at a graph break, graph
  variables and hot reload with migration by identity.
- `editor-architecture`: the gameplay graph debugger and hot reload in the editor.

## Impact

- Engine: `src/graph/` (`script_debug.h/.cpp`, `script_reload.h/.cpp`; `ScriptOp::Probe`,
  `RunOutcome::Paused`, `ScriptState`'s pause point, `Variable`, `ProbeSite`, the debug hook on
  `execute*`; `ProgramBuilder` moved to `script_build.h`; three node types), `src/game_backend/`
  (`GraphBehaviours`' debugger and reload), `src/editor_backend/` (four operations),
  `samples/05b-editor-window/runtime/` (`GraphRuntime` holds the session; Play's resume continues a
  held graph).
- Editor: `cy-editor-services` (`script_debug`, `script_debug_commands`; `ScriptRequests` keeps
  breakpoints, watches, debug state and reload replies), `cy-editor-commands` (eight defaulted
  `ProjectHost` methods), `cy-editor-shell` (the canvas's gutter and marks; the panel's debugger).
- No ABI or bridge message change; existing program digests are unchanged (the probe opcode is
  appended, and a program without variables hashes as before).
