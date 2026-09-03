# Design: diagnostics, abilities, and graphs

## 1. Thirty-nine diagnostics requirements, one transport

Nearly every capability already ends with a diagnostics requirement, and each is well chosen: the
job system reports critical paths, residency answers *why is this not resident*, illumination answers
*why is this area dark*, generation answers *why is this tree here*. The engine's culture of causal
diagnostics is one of its better properties.

What none of them share is a transport. Left alone, each would grow its own event format, its own
buffering, its own capture file, and its own viewer, and correlating a streaming stall with a task
graph stall with a memory spike would be impossible because the three would be recorded in three
incompatible ways.

So this capability owns **the trace**, and every subsystem's diagnostics requirement becomes a
statement about *what it emits* rather than *how*. That is the same shape as the residency layer
owning policy while subsystems keep storage, and the field substrate owning data while subsystems
keep meaning.

## 2. Telemetry must never be the reason a frame is slow

A profiler that allocates per event, takes a lock, or blocks a worker changes the thing it measures
and can destabilise a shipping build.

So: compiled event identifiers rather than strings in the hot path, per-thread buffers with no shared
lock, no allocation per event, and a **declared loss policy** — verbose channels are dropped first,
breadcrumbs and tick boundaries are preserved last, and the number dropped is recorded so the gap is
visible rather than silent.

The real-time audio thread and the simulation are never made to wait for a consumer.

## 3. The profiler cannot be started after the problem

The recurring failure of profiling workflows: a hitch happens, someone attaches a profiler, and the
hitch does not recur.

A **rolling buffer** is always on at low cost, and a hitch, a budget overrun, an assertion, or a fault
**freezes it** — the seconds before and after — and writes a capture. Combined with the crash replay
buffer `replay-and-rollback` already specifies, a bug report can carry both what the engine was doing
and how to make it do it again.

That combination is the single most valuable thing in this capability, and it exists only because the
replay work was done first.

## 4. Crash artefacts must survive the absence of everything

A crash report is produced at the worst possible moment: the process is damaged, the editor is not
attached, the player has no symbols, and the GPU may be gone.

So the artefact is self-contained and symbol-independent — build identity, module offsets,
breadcrumbs, the trace tail, the log tail — and symbolication happens later, wherever the symbols
were archived by the build system. Device loss carries the render graph state, the last passes, and
the pipeline and resource identifiers, because "device removed" on its own is not a diagnosis.

## 5. Abilities: the module the framework promised

`gameplay-framework` deliberately excluded abilities so that a game without them pays nothing. This
supplies them on the terms that exclusion implied: an optional module, activatable as a gameplay
feature, built on commands, tags, validation, time, and the determinism model rather than beside
them.

The runtime shape follows the engine's existing answer to this problem, for the sixth or seventh
time: a **compiled program shared by every owner**, and compact per-owner state. Ten thousand units
with ability sets are ten thousand small records — an ability identifier, a cooldown tick, a charge
count — processed by systems over archetypes, not ten thousand ability objects with virtual
activation.

## 6. Two details that decide whether an ability system is usable

**Modifier order must be specified, not conventional.** Additive before multiplicative before
override before clamp — or whatever the engine chooses — but written down, because every game with
attributes eventually has two modifiers whose result depends on order, and a convention produces two
different answers on two machines.

**Stacking is a declared policy**, not something each effect implements: stack, refresh, replace,
highest, lowest, unique by source, limited count. Poison stacking to five and refreshing its duration
is the same problem in every game that has poison, and solving it once is the point of a module.

**Cooldowns are tick values, not float timers**, so they are exact, reproducible, and rollback-safe —
consistent with the simulation clock rather than a parallel notion of time.

## 7. Prediction needs identity

A client predicts an ability, the server confirms or rejects it, and the client must reconcile —
which requires knowing *which* activation the server is answering.

So an activation carries an **identity**, used for reconciliation, for suppressing duplicate cues
through the side-effect ledger, for rollback, and for network debugging. Without it, prediction
reconciliation is heuristic matching, which fails exactly when two similar activations happen close
together.

Cues are presentation and sit on the presentation side of the determinism firewall: a cue may be
speculative, a currency deduction may not.

## 8. Graphs: shared infrastructure, separate languages

The tempting design is one universal graph system that materials, VFX, animation, AI, gameplay, and
abilities all use. It is wrong, and the engine has already demonstrated why: a material graph lowers
to closures and shader code, a VFX graph to simulation kernels, an animation graph to pose
evaluation, an AI graph to behaviour programs. Those are different languages with different type
systems and different execution models. Forcing them through one intermediate representation would
make each worse.

What they genuinely share is everything *around* the language: nodes and pins, typed connections,
stable node and pin identity, serialization, subgraphs, the editor canvas, undo, diffing, versioning,
and debugging. Five capabilities have each built some of that.

So CyberGraph is **shared frontend infrastructure with domain-specific lowering**, and the new
languages it adds are gameplay and ability graphs, which lower to ECS systems and ability programs.

## 9. Graphs are an authoring language, and that is the whole point

The failure mode of visual scripting is that the graph becomes the runtime: an interpreter per
entity, a node object per node, a virtual call per pin, and a tick event as the default idiom. That
produces exactly the execution model this engine exists to avoid, wearing a friendly face.

So: typed pins rather than a universal variant, so errors are compile-time; **events rather than tick
as the default**, with continuous update available and explicit; per-entity graph state compiled to
**generated component data** rather than hidden interpreter state; and lowering to an ECS system
iterating chunks, so a hundred thousand entities with a door behaviour cost one system.

Two backends serve two purposes: a portable **bytecode** for fast iteration, hot reload, sandboxed
mods, and stepping; and a **native** path for shipping. The bytecode is a shared program with
separate state, never an instance per entity.

## 10. What a graph compiler can enforce that review cannot

This is where visual scripting can be better here than elsewhere rather than merely equivalent.

A graph marked deterministic can be **statically audited**: wall-clock reads, ambient randomness,
iteration over unordered containers as a decision order, reads of presentation-classified data, and
disallowed floating-point operations are all visible in the intermediate representation. Handwritten
code needs a lint that may be incomplete; a graph compiler sees the whole program.

The same applies to **capabilities**: a mod graph can be restricted to gameplay operations with no
file, network, or asset-mutation access, enforced at compile time rather than by trust.

## 11. Diff and merge is where visual scripting usually fails a team

Graphs are notoriously painful in source control, and the reason is representational: opaque binary,
unstable identifiers, and visual position stored with semantics, so moving a node produces a diff.

So node and pin identity is stable, **layout is stored separately from semantics**, the source form is
deterministic text, and diff and merge are **semantic** — node added, connection changed, default
changed, node moved as a visual-only change. Three-way merge understands topology.

This is a solvable problem that most implementations do not solve, and solving it is worth more to a
team than any number of extra node types.

## 12. Order, and why diagnostics is first

| Phase | Contents |
|---|---|
| 1 | Trace identifiers, per-thread buffers, channels and loss policy |
| 2 | Task, ECS, memory, GPU and IO views; structured logging |
| 3 | Rolling buffer, hitch capture, capture artefact |
| 4 | Crash artefacts, breadcrumbs, device loss, symbolication, privacy |
| 5 | Reproduction linking the crash artefact to the replay buffer |
| 6 | Attributes, ability and effect state, validation, costs and cooldowns |
| 7 | Targeting, modifiers and stacking, effect scheduling |
| 8 | Command integration, prediction, rollback participation, AI use |
| 9 | Graph infrastructure: nodes, pins, identity, serialization, diff |
| 10 | Graph intermediate representation, compiler, bytecode and native backends |
| 11 | Gameplay and ability graph frontends lowering to systems and ability programs |
| 12 | Graph debugging, determinism auditing, capabilities, tests |

**Diagnostics comes first because everything after it is easier to build with it than without it** —
including the two capabilities in this same change. And the graph infrastructure precedes the ability
graph frontend deliberately, so that abilities adopt the shared editor rather than growing a sixth
bespoke one.

## 13. Non-goals

- **One universal graph intermediate representation.** Domain languages keep their own lowering.
- **Graphs as the authoritative runtime model.** They are a source language.
- **Sandboxing native code through graph capabilities.** Capability restriction applies to graphs;
  native plugins remain a trust decision, as `project-and-plugins` already states.
- **An ability system every game must use.** It is a module, and a game without abilities links none
  of it.
