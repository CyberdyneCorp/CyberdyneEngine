# `src/graph/` — CyberGraph

*One authoring layer, one expression core, and a compiler per consumer.* M8.b section 2.

## What this module is, and what it deliberately is not

M8.b's proposal opened with "One graph IR, proven against every consumer before six are built on
it." **The milestone's own spike refuted that premise before a line of this module was written**, and
`openspec/changes/implement-m8b-systems/design.md` §1 records the measurement: five of seven
consumers would need an escape hatch against a stated budget of two. `visual-scripting`'s first
requirement had already forbidden it by name —

> Visual authoring SHALL be provided as **shared graph infrastructure** with **domain-specific
> lowering**, not as one universal graph language. […] **Scenario: No universal representation** —
> WHEN a proposal would route material expressions and gameplay control flow through one
> intermediate representation, THEN it SHALL be rejected against this requirement.

So this module is three separable layers, and the middle one is smaller than the plan expected:

| Layer | Header | What it is |
|---|---|---|
| The authoring layer | `cybergraph.h`, `text.h`, `merge.h`, `audit.h` | Nodes, typed pins, typed connections, stable identity, layout as a side table, a deterministic textual source, subgraphs, semantic diff and three-way merge, versioning and migration, opaque preservation, node- and pin-precise diagnostics, the debug map, capability sets and the determinism audit. **Every consumer adopts it.** |
| The expression core | `expr.h`, `passes.h`, `emit.h` | A hash-consed pure-expression SSA DAG whose identity is a content hash — `src/rendering/material/`'s IR generalised by an open type lattice, an open operation table, declared roots and a declared phase boundary. **Only the consumers whose values are pure expressions use it.** |
| One lowering per consumer | `lower_script.h`, `lower_pose.h`, `lower_behaviour.h`, `lower_camera.h` | The form each consumer's own specification names. All compiling; none interpreting. |

`lower_script.h` carries **both execution backends** `visual-scripting` requires from one
intermediate representation — the bytecode register machine in `lower_script.cpp` and the
ahead-of-time-resolved native path in `lower_script_native.cpp`. They share `ScriptState`, so an
instance suspended under one resumes under the other, and `integration.graph_compiler` checks that
they agree effect for effect rather than only on the outcome.

## Which consumer uses which, and why

| Consumer | Lowers to | Why not the expression core |
|---|---|---|
| `visual-scripting`, `gameplay-abilities-and-effects` | `lower_script.h` — basic blocks, control flow, calls, events, commands, queries, suspension points, over a typed register machine | Every item on that list is a back edge, a write, or an ordering. The core has none by construction |
| `animation-and-skinning` | `lower_pose.h` — poses as values, a lazily-evaluated state machine, pose dependency analysis | "Lower-body joints of that layer's clips are never read, and they SHALL NOT be sampled" needs a lazy branch; the core's conditional evaluates both arms |
| `ai-system` | `lower_behaviour.h` — a flat instruction stream and a parameter table over one shared register machine | A three-valued status, a saved program counter, a written blackboard, and a runtime search |
| `camera-system` | `lower_camera.h` — **on the expression core** | Every rig node is a pure function of its inputs. It is the first user of the open type lattice, of declared roots and of the phase boundary |
| `material-compiler` | `src/rendering/material/` — **unchanged** | M7's closed work. See below |
| `vfx-system`, `sequencing-and-cinematics` | M8.c | Deferred with the rest of that milestone |

## `src/rendering/material/` is not touched, and the anchor is why

An open operation table invalidates every material cook key, and re-testing M7's closed work
mid-milestone buys nothing. Its criterion here is an **anchor** instead: `tests/anchor/` expresses
the material op table and type lattice as a `cy::graph::Domain` over this core and compiles the
tree's own reference material, which must reproduce three digests the spike measured against the
real compiler:

| | Digest |
|---|---|
| IR, 26 authored nodes → 26 IR nodes | `f48f3faf395e52fd` |
| After the pipeline: 16 nodes, 3 dropped, 10 merged, 1 folded | `178a3630921e0506` |
| The emitted program, 11 statements | `7f74500626ea001a` |

A core that hits all three has lost nothing the material compiler depends on, and porting it later
is a mechanical change rather than a risk inside this milestone.

**The anchor and the open operation table are in direct tension, and the resolution is visible.**
Task 2.2 requires operation and type identity to move to TEXT, because `ir.h` says of itself that
"the enumerator VALUES are part of every content hash and therefore of every cook key" — and a table
two domains extend has no stable positional identity. Task 2.3 requires reproducing a digest
computed from those very enumerator values. The core resolves it with `pinned_identity`, and
**`tests/anchor/` is the only domain in the tree that uses it**. Every other domain, the camera rig
included, hashes its operations by name. Dropping the pin is what a real port of the material
compiler would do, and it invalidates that module's cook keys once, exactly as the extension said it
would.

## A digest may only close over bytes something wrote

`ScriptProgram::digest()` is a cook key and the back-end selection key `visual-scripting` requires
to be stable, and for most of M8.b it was neither: `finish_digest` hashed each constant with
`hash_bytes(&constant, sizeof(constant))`, and `script::Value` is thirty-two bytes of which four —
between `z` and `handle` — are written by no member initialiser. A byte dump found a fragment of a
spilled stack address sitting in them, so the same authored graph compiled to a different digest in
every process, and to none at all reproducibly under `--profile release`.
`samples/08-vertical-slice` found it by compiling one graph three times; M8.b's closing gate fixed
it.

The rule the fix leaves behind, because it is not specific to this struct:

- **`script::hash_constant` hashes the five fields.** Nothing here may hash a `Value` as an object.
- **`Immediate` is still hashed as raw bytes in three places** — `Builder::hash_of`,
  `hash_literal` and the rig digest — and that is sound only while its layout is flat. `expr.h`
  carries a `static_assert` on `sizeof(Immediate)` beside a note saying so, so a member that
  introduces padding is a compile error rather than a cook cache that serves the wrong artefact.
- **`test_lowering.cpp` holds the property** by comparing the program's own digest against the same
  sum recomputed field by field, having first asserted that the padding really is dirty. Restoring
  the old line was run, and the case goes red.

## What must not be retrofitted

- **One front end, adopted by every consumer.** Seven graph editors with seven diff formats and
  seven debuggers is the failure `visual-scripting`'s shared-infrastructure requirement was written
  against.
- **No graph is interpreted at runtime.** Every program here is immutable and shared; every
  instance is a register file and a few integers. `visual-scripting` permits a shared register
  machine by name — "There SHALL NOT be one virtual machine instance per entity" — and forbids the
  alternative.
- **A domain's IR is extended deliberately, never patched.** The four extensions are specified in
  `design.md` §1.4 before they were written, and `expr.h` names each of them at the site that
  implements it.
- **Layout is not meaning.** `Graph::semantic_digest()` closes over nodes, pins, links, properties
  and the interface; moving a node on a canvas must not recompile anything and must not conflict in
  a merge.
- **An audit that cannot examine a node reports INCOMPLETE, not clean.** A gate that treats "I could
  not look" as "nothing found" goes green on the day a plugin fails to load.

## Suites

| Suite | Kind | Subject |
|---|---|---|
| `unit.cybergraph` | unit | The authoring layer: identity, typed pins, the textual round trip, diff and merge, migration, the audit |
| `integration.graph_compiler` | integration | The expression core, the anchor, and the four lowerings. Integration because a case here runs the optimisation pipeline to a fixed point over a 26-node material several times and emits its program — the shape M7's own suite learnt does not fit the unit tier's millisecond in a Debug configuration |
