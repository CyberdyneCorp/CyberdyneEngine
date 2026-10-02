# Design: the Play debugger and hot reload (#84 stages 2 and 3, #29 tasks 4.1 and 4.2)

## Context

CyberGraph compiles a gameplay graph to a typed register machine: one immutable `ScriptProgram` shared
by every instance, one `ScriptState` per instance, a bytecode loop and a native back end that resolves
each opcode to a function pointer ahead of time. The compiler already emitted a `DebugMap` from program
location to node. Play runs graphs through `cy::game_backend::GraphBehaviours`, ticked by the hosted
runtime after the Swift behaviours, inside `PlaySession::advance_one`.

## Decisions

### Probes in a copy of the program, not a hook in every instruction

`instrument_for_debug` copies a compiled program and inserts `ScriptOp::Probe` before each node's
instructions. Everything else is copied unchanged: the blocks keep their ids, the registers their
numbers, the suspend points and externals their order. An instance's `ScriptState` is therefore valid
against the plain program and the probed one, and `GraphBehaviours` switches between them at a tick
boundary without touching state. A probe's operand indexes `ScriptProgram::probes()`: the node, the
block, the probe's offset in the block, and whether the node is on an execution chain.

A node's instructions are not always contiguous (an unwired argument's zero is loaded before the other
operands are evaluated, and a chain that runs out ends with a `Return` recorded against its first node),
so a node is probed before the run that holds its last instruction other than a `Return`. Each event
handler's first block also gets a probe naming its `script.on_event` node, which compiles to nothing, so
a breakpoint on the event stops before the handler's first node.

**Cost.** A program the compiler emits has no probe; given a hook anyway it never calls it (tested).
The bytecode loop tests for `Probe` under `if constexpr (kGraphDebuggerEnabled)`, which follows
`CY_DEVELOPMENT`; in Profile and Shipping the test is not compiled, the native probe step is empty and
`instrument_for_debug` refuses, so no shipped program can hold a probe and no shipped loop looks for
one. The opcode is appended after the terminators, so no existing opcode changes number and no existing
program digest moves; the committed compile fixture is unchanged.

The alternative — a per-instruction callback compiled only into debug builds — would cost every
development run a test per instruction whether or not anyone is debugging, and would not exist on the
native back end without a second mechanism.

### A pause is a block and an offset, like a suspension

A hook answering `Break` leaves the instance paused: the run returns `RunOutcome::Paused` and the state
records the block and the offset after the probe. `execute` / `execute_native` continue a paused instance
there, with its registers as they were (nothing is persisted or restored). Both back ends record the
pause in the bytecode back end's terms, as they already do a suspension, so the same breakpoint stops at
the same node with the same state on either.

### Pins through the debug map

The compiler now records the pin an instruction writes: a node's output `value`, a call's packed `arg0`
/ `arg1`, an unwired input's zero, a variable write's `value`. `read_pin` finds the instruction recorded
against the node and pin and reads its destination register; a data node evaluated in more than one
block has a register in each, and the block the instance is paused in is preferred.

### What pauses: the whole simulation tick

A breakpoint inside the graph tick cannot block the thread (the runtime answers the editor on it), so the
tick is made resumable. `GraphBehaviours` remembers where its work stopped — which instance, in a raise
or in the resume pass — and refuses `update` and `raise` while paused. `debug_continue` and `debug_step`
finish the paused handler, then run the held work on in its original order. The hosted runtime pauses
its `PlaySession` when a graph holds (so physics, the Swift behaviours and the clock stop; ticking a
paused session does nothing) and Play's audio through a hold listener, and resumes the session when the
held tick is finished — only if it was running when the graph broke. Play's own resume continues the
held graph rather than starting a tick over an unfinished one. This is what other engines do: a
Blueprint breakpoint stops the game thread.

Determinism follows: nothing outside the paused tick advances, and the held work runs as it would have.
`integration.game_backend_graph` runs three units with breakpoints, steps and continues against an
undebugged twin and requires the same placements and cues every tick; `integration.editor_window_graph_
debugger` does the same through a real `PlaySession`.

### Stepping is per instance and stays armed

A step arms the paused instance: `Into` stops at its next probe, `Over` at its next probe on an
execution chain. Across a wait the step stays armed and stops when the instance resumes, a later tick if
need be; a breakpoint another instance reaches first still stops there.

### Variables and their identity

`script.variable` declares per-instance state; the compiler reserves a register for each variable in
every block, initialises it in `ScriptState`'s constructor, and lowers `script.get_var` / `script.set_var`
to a `Move`. A variable's identity is its declaring node's key — author-owned and stable across edits —
so a renamed variable keeps its value across a reload, and one deleted and declared again starts over.
A program without variables hashes exactly as before.

### Reload: refuse before anything moves, swap at a tick boundary

`GraphBehaviours::reload` compiles and binds the new source and runs `check_migration`; a compile error
or a variable whose kind changed refuses the reload with diagnostics on the nodes and changes nothing.
An accepted reload is staged and applied at the start of the next `update`, to every instance of the
graph at once; the graph's generation moves on. `migrate_state` carries variables by identity, starts
new ones at their default and drops removed ones. A wait in progress continues when the new program has
the same wait node and carries nothing across a wait but variables; otherwise the instance becomes idle
and the report says so. An instance paused mid-handler is never migrated: the reload waits for the held
tick to finish.

### The editor keeps the breakpoints; the engine holds the pause

Breakpoints are the editor's (`ScriptRequests` keeps the wanted set), so they can be set before Play;
the first debug state that reports Play running sends them. While the panel is drawn the debug state is
polled with Play's state, carrying the watch list, so the engine reads exactly the pins the panel shows.
The gutter shows the union of the wanted and the engine-held breakpoints. A save, an undo or a redo of a
graph Play runs sends `script.reload`; a graph Play does not run is not sent.

## Not in this slice

Per-node profiling and a heat map of cost; predicted-versus-authoritative views; declared migration
policies other than keep-by-identity (reset, restart); semantic diff and merge; Swift interop; AI and
ability graphs in the panel.
