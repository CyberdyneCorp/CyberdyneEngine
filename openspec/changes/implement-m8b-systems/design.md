# Design: M8.b — Systems

## 1. The spike, and the failure budget it carries

**Section 1 is closed. The spike was run, and its premise did not survive it.** What it found is
recorded from §1.1 down so the implementing agents do not re-derive it; §5 says where it lives and
how to re-run it. The question and the budget are left exactly as they were commissioned, above the
result, because a budget rewritten after it was spent measures nothing.

**The spike is one question asked seven times: can a single graph IR express what each consumer
needs, and what does it cost the consumer that fits worst?** The seven are `visual-scripting`,
`gameplay-abilities-and-effects`, `ai-system`, `animation-and-skinning`, `sequencing-and-cinematics`,
`vfx-system` and `camera-system`. The starting position is not a blank page: `src/rendering/material/`
already compiles a graph to an IR to closures to a program, it is the only compiled-graph
implementation in the tree, and the roadmap ordered the material compiler before every other graph
consumer for exactly this reason.

**The failure budget, stated before the work rather than after it.** The split that created this
milestone put the spike's risk inside it rather than in front of the authoring work, and a risk
inside a milestone needs a stated price:

| Spend | What it buys | What it costs |
|---|---|---|
| The IR carries a consumer's semantics as written | that consumer lowers through it | nothing |
| The IR is **extended** to carry them | the extension is one change to one module | every consumer already built on it is re-tested; the extension is specified before it is written |
| The IR **cannot** carry them and the consumer needs an escape hatch | that consumer keeps its own evaluator behind the same authoring surface | the "no graph is interpreted at runtime" exit criterion is narrowed **by name**, in `ROADMAP.md`, to the consumers it still holds for |
| Two or more consumers need escape hatches | the shared-IR premise is wrong | the milestone is re-planned rather than delivered on a premise its own spike refuted |

**Two escape hatches is the budget.** It is a number so that exhausting it is an observation rather
than an argument, and it is two rather than one because a single ill-fitting consumer is a normal
outcome of designing an abstraction against six.

**The spike runs before the implementing work and its report is committed**, the way M7's two were.
What it must produce: for each of the seven, the semantics it needs stated as a list, whether the IR
expresses each one, and — for anything it does not — which row of the table above that spends.

---

### 1.1 The result: the premise is refuted, with margin

**`~/cyberdyne-spikes/m8b-ir-spike` — 13 probes against `src/rendering/material/`'s IR, 6 of them
recording a semantic it cannot carry.** Identical under `clang++ 18.1.3` and `g++ 13.3.0`, at `-O0`
with ASan and UBSan and at `-O2`; all four builds print byte-identical output, and the probe derives
its exit status from its gaps rather than choosing one beside them, so it exits 1.

The headline is a count over a fixed probe set, not an extreme value: **five of the seven consumers
need an escape hatch under §1's table, against a budget of two.** The premise is refuted three times
over, and it was refuted before a line was measured, because the answer is already written in a
committed specification.

**`visual-scripting`'s spec forbids the thing this milestone was going to build.** Its first
requirement is *"Shared infrastructure, domain-specific languages"*, and it is normative:

> Visual authoring SHALL be provided as **shared graph infrastructure** with **domain-specific
> lowering**, not as one universal graph language. […] Existing domain graphs — materials, effects,
> animation, artificial intelligence, and camera rigs — SHALL keep their **own intermediate
> representations and compilers**, because a material's algebra, a particle kernel, a pose
> evaluation, and a behaviour program are different languages with different type systems and
> execution models. Forcing them through one representation would make each worse.
>
> **Scenario: No universal representation** — WHEN a proposal would route material expressions and
> gameplay control flow through one intermediate representation, THEN it SHALL be rejected against
> this requirement.

Its "Forbidden visual scripting patterns" list closes with *"A universal intermediate representation
forced onto domain languages"*. `proposal.md`'s "One graph IR, proven against every consumer before
six are built on it" is that proposal, and this specification rejects it by name.

That is the cheap half of the answer and it cost a reading. The measured half is below, and it
matters anyway: it says **which** parts are shared, **which** consumers can still ride the material
IR with a specified extension, and **what each of the others actually needs** — which is what the
implementing agents build from, and what a reading alone does not give.

### 1.2 Task 1.1 — what each consumer's specification requires of an authored graph

Read per consumer, from its own specification, not generalised from one.

**`visual-scripting`** — *"a typed intermediate representation with **basic blocks**, **explicit
control flow**, typed values, field access, **calls**, **event emission**, **command emission**,
queries, and **suspension points**"*. Pins carry *"booleans, integers, floating-point values,
vectors, **entity references**, **persistent references**, **asset handles**, **gameplay tags**,
identifiers, **structures**, **arrays**, and **optionals**"*, and *"a universal variant type SHALL
NOT be the default pin type"*. Two back ends from one IR — **bytecode** (a typed register machine,
one shared program, separate state) and **native**. Compilation emits the **data access
declaration** the scheduler needs. Asynchronous waits lower to *"an explicit state machine with
compact generated state"*. Plus determinism auditing, capability sets, semantic three-way merge, hot
reload with state migration, node versioning with opaque preservation of missing nodes, and a debug
map from program location to node and pin.

**`gameplay-abilities-and-effects`** — an **ordered pipeline**, stated as an order: *"resolve owner
and context, check state and tag requirements, check cost, check cooldown, resolve and validate the
target, apply the prediction and authority policy, commit the activation, apply effects, and emit
cues and events"*. **Ticks, not floats**: *"cooldowns SHALL be expressed as ticks, as a ready-tick
value rather than a counting float timer"*. **Transactional** cost — validate, reserve, commit.
Modifier evaluation in an **engine-specified order** with stable tie-breaks. A **structured
validation result** callable *without activating*. Waits compiled to *"an explicit state machine with
compact state"*, with declared cancellation causes. Randomness from *"a stream derived from
activation identity, ability identity, and the session seed"*. Its graph language is
`visual-scripting`'s: that spec *"SHALL additionally provide gameplay and **ability** graph
languages, lowering to ECS systems and **ability programs** respectively"*.

**`ai-system`** — one asset composing **four reasoning models nested arbitrarily**: hierarchical
state tree, behaviour tree, utility scoring, GOAP. Behaviour-tree status is a **three-valued
`Running`/`Success`/`Failure`**, and *"execution resum[es] at the running node rather than
re-descending from the root each tick"* — so an unselected subtree must cost nothing. Utility
scoring needs **response curves (linear, quadratic, logistic, inverse)** and hysteresis. GOAP is a
**runtime search** with a budget, incremental across ticks, invalidation and replanning. The compiled
form is *"a flat instruction stream plus a parameter table"* with per-agent state of *"program
counter, execution stack, timers, and blackboard"* — a **shared bytecode VM**, and the blackboard is
**written** by nodes.

**`animation-and-skinning`** — *"graph → typed IR → optimisation → compact program"*, over values
that are **poses**: *"sample active clips into poses, blend poses per the compiled animation program
into a final pose"*. Nodes include a **`StateMachine`** — *"states with transitions, conditions,
durations, and **interruption** rules"* — and **`Layer`**, **`BlendMask`**, **`Additive`**, **`IK`**.
The optimiser must do **pose dependency analysis**, whose scenario is explicit: *"lower-body joints
of that layer's clips are never read, and they SHALL NOT be sampled"*. Sync groups align by **marker
correspondence** rather than normalised time. Root motion is gameplay state on a deterministic CPU
path. Motion matching is a **runtime nearest-neighbour search** over a pose database.

**`sequencing-and-cinematics`** — **not a node graph at all**: *"tracks containing sections over time
ranges, containing channels of typed keyed data"*, compiled to *"sorted evaluation segments, binding
tables, resolved property accessors, compact channel data, **event tables**, a **preload plan**,
dependency metadata, and a debug map"*. Time is **exact** — *"a frame and subframe at a declared
rational rate, and SHALL NOT be an accumulated floating-point value"*. Channels are typed with
**rotation interpolating as an orientation**. The compiler builds **interval and event indexes** so
cost scales with active sections. Authoritative change is **ordered commands**, not property writes.
Sections declare priority, weight, blending, pre-roll, post-roll and loop behaviour.

**`vfx-system`** — *"graph → typed **VFX IR** → optimisation → **Slang** source"*, and *"the IR SHALL
be typed and **SSA-formed**"* with *"attribute liveness analysis, dead-code elimination, constant
folding […] and **kernel fusion**"*. Storage is **structure-of-arrays with the attribute set derived
by the compiler** from what the graphs read and write, at compiler-selected precision. Reads come
from extensible **typed data interfaces**. This is the material IR's own shape, one step over.

**`camera-system`** — *"a **graph of rig nodes** — target, position, orientation, constraint,
collision, lens, noise, blend, and output — composed rather than inherited"*, *"compiled at cook time
into a compact rig program"* with *"no per-node allocation or virtual dispatch"*. Smoothing must be
**frame-rate independent in physically meaningful terms — half-life, or frequency and damping
ratio** — with *"smoothing state […] part of the rig instance"*, resettable on a cut. Collision and
occlusion queries are **batched through the physics interface** rather than *"scattered synchronous
casts from individual rig nodes"*. Shake is an **additive impulse bus**. Output is a pose, a lens and
derived data.

### 1.3 Task 1.2 — measured against `src/rendering/material/`'s IR

Thirteen probes. Each one runs; none asserts. `out/probe.txt` is the transcript the figures below are
quoted from.

| | What was measured | Result |
|---|---|---|
| P1 | The type lattice against the pin types the seven specs name | `ValueType::Count = 7`; **15 of 20 required pin types absent** — Rotation, Transform, Pose, EntityRef, PersistentRef, AssetHandle, GameplayTag, Struct, Array, Optional, Enumeration, ExactTime, Tick, BTStatus, TargetData |
| P2 | The operation set and its control flow | `Op::Count = 31` (21 non-closure, 10 closure, 5 commutative); one conditional, `Select`, arity 3; **14 of 14** named transcendentals absent — Sin, Cos, Exp, Log, Sqrt, Abs, Floor, Mod, Atan2, Cross, Slerp, Step, Smoothstep, Clamp |
| P3 | A back edge — a `StateMachine`, a resumable tree, a camera carrying state | `InvalidArgument / "an operand names no node in this module"`. No `Phi`, `Loop`, `Block`, `Jump` or `Call` op exists |
| P4 | Two reads of one mutable cell at two instants | Both are **NodeId 0, hash `a40053c293fb9992`** — one value. No `Store`, `Write`, `Assign` or `Set` op exists |
| P5 | Two effects applied in a stated order | `"A then B"` and `"B then A"` are both **NodeId 4**. `op_is_commutative(ClosureAdd) = true`; the order was erased |
| P6 | A statement whose value nothing reads | `optimise()`: **5 → 2 nodes, dropped 3**, dropped authoring id `4242` |
| P7 | The roots | `set_surface(a Float)` → `InvalidArgument / "the surface root must be a closure"`. Two roots, fixed: a surface closure and an opacity. **Not one of the seven wants either** |
| P8 | The subset that fits | VFX colour-over-life: **7 nodes, digest `87a48486e48540ec`**, 515 bytes of Slang |
| P9 | `Op::Custom`, the IR's own hatch | Accepts a logistic curve as **Slang text**. Available to the two consumers whose programs are shaders; useless to the five whose programs are CPU code |
| P10 | Is `Select` lazy? | **No.** After every pass: 8 nodes, 6 statements, **2 texture samples**, 1 branch; both clips appear in the emitted body |
| P11 | The authoring layer above the IR | `GraphOp`: **24 enumerators, 10 material-specific, 14 domain-neutral**; stable authoring id, mute, side-table flags, wire model, provenance-as-a-set, drop report and debug map carry no material semantics at all |
| P12 | The two candidates, encoded for real | `position' = position + velocity*dt` **compiles**, 6 nodes, digest `f8894c035f1b0e4b`; `colour' = tint*(1-age/lifetime)` **compiles**, 7 nodes, digest `20025811937c0ee9`; `lerp(prev, desired, 1 - base^(dt/hl))` **compiles**, 8 nodes, digest `7dd08230a50f73ce` |
| P13 | The anchor, from the tree's own reference material | 26 authored nodes → IR **26 nodes, digest `f48f3faf395e52fd`** → pipeline **16 nodes, digest `178a3630921e0506`** (dropped 3, merged 10, folded 1) → program digest `7f74500626ea001a`, 11 statements |

**P3 through P7 and P10 are not gaps in a table; they are the data structure.** The material IR is a
bottom-up hash-consed pure-expression DAG whose identity is a content hash. Every one of those six
properties follows from that sentence, and each is exactly what makes it good at its own job:

- an operand must already exist, so there is no back edge — and no state machine, no resumable tree,
  no loop and no basic block;
- identity is content, so two reads of one mutable cell **are** one read;
- canonicalisation sorts commutative operands by hash, so an authored order does not survive;
- optimisation is a rebuild from the roots, so a value nothing reads is deleted;
- the roots are a surface and an opacity, and their types are not a parameter;
- `Select` is a value, so both arms are evaluated.

The last of those is the sharpest, because two specifications forbid it in so many words:
animation's *"they SHALL NOT be sampled"* and ai-system's *"resume at that task without
re-evaluating the whole tree"*. Both require a **lazy** branch. P10 emits both clips.

### 1.4 Task 1.3 — the budget, spent

Named, per §1's table, one row per consumer.

| Consumer | Row spent | Named |
|---|---|---|
| `vfx-system` | **Extension** | E1, E2, E3 |
| `camera-system` | **Extension** | E1, E2, E3, E4 |
| `visual-scripting` | **Escape hatch** | H1 |
| `gameplay-abilities-and-effects` | **Escape hatch** | H2 (served by H1's IR) |
| `ai-system` | **Escape hatch** | H3 |
| `animation-and-skinning` | **Escape hatch** | H4 |
| `sequencing-and-cinematics` | **Escape hatch** | H5 |

**The extensions**, each one change to one module, each specified here before it is written:

- **E1 — an open type lattice.** `ValueType` becomes a domain-supplied table (name, component count,
  the arity rules the checker reads) instead of a fixed enumeration of seven. The type's identity
  enters the content hash **by text**, for the reason `ir.h` decision 2 already gives about `Name`.
  Buys: Transform, Quaternion, Tick, and whatever a domain adds. Re-tests: material.
- **E2 — an open operation table.** `Op`, `op_arity`, `op_is_commutative` and `op_is_closure` become
  a domain-supplied descriptor table. This one has a trap `ir.h` states itself — *"the enumerator
  VALUES are part of every content hash and therefore of every cook key"* — so op identity must move
  to text at the same time, or every cook key in the tree becomes a function of enumerator order in
  a table two domains now extend. Buys: transcendentals, and each domain's own vocabulary.
  Re-tests: material, and invalidates its cooked data once.
- **E3 — declared roots.** `Module::surface()`/`opacity()` become a domain-declared list of typed
  roots. Buys: VFX's one root per written attribute, camera's pose-plus-lens, and the
  transfer-function shape P12 demonstrates, where per-frame state is carried **in** as an input and
  **out** as a root. Re-tests: material, whose two roots become a two-entry list.
- **E4 — a declared phase boundary.** One DAG splits into two dispatches at a point where an external
  batched result arrives. Camera needs it for *"queries SHALL be batched […] rather than issued
  individually per node"*; VFX's kernel fusion is the same boundary read from the other side.
  Re-tests: material, which declares none and is unaffected.

**The escape hatches**, each a consumer that keeps its own intermediate representation and compiler:

- **H1 — `visual-scripting`.** Needs basic blocks, explicit control flow, calls, event and command
  emission, queries, suspension points, ten pin types with no `ValueType`, and two back ends of which
  neither is a shader. Nothing in that list is an extension of an expression DAG; it is a different
  data structure. Its own specification says so, and says it first.
- **H2 — `gameplay-abilities-and-effects`.** Needs an ordered pipeline (P5), effects whose value
  nothing reads (P6), tick arithmetic (P1), a transactional commit and a structured validation
  result. **It spends no new IR**: `visual-scripting`'s spec already assigns it the ability graph
  language lowering to ability programs, so H1 and H2 are one representation serving two consumers.
- **H3 — `ai-system`.** Needs a three-valued status, a program counter and an execution stack, four
  nested reasoning models, blackboard writes (P4) and a **runtime GOAP search**. Its compiled form is
  a flat instruction stream, which is a bytecode, which is not a DAG.
- **H4 — `animation-and-skinning`.** Poses as values (P1), a state machine with interruption (P3),
  and pose dependency analysis, whose whole content is that an unselected branch must not be
  evaluated (P10).
- **H5 — `sequencing-and-cinematics`.** It is not a node graph. Its source is tracks, sections and
  channels over exact rational time, and its program is an interval index, an event table and a
  preload plan. A graph IR is the wrong shape for it in both directions.

**Five hatches against a budget of two.** §1 says what that means, and it means it: *"the shared-IR
premise is wrong […] the milestone is re-planned rather than delivered on a premise its own spike
refuted"*. Say it plainly: **the premise is refuted.** The number is five and not three or four, so
it is an observation and not an argument.

### 1.5 The row the budget table does not have, and why it changes the conclusion

§1's table has three rungs — the IR expresses it, the IR is extended, or the consumer *"keeps its own
evaluator"* — and the price of the third is that **the "no graph is interpreted at runtime" exit
criterion is narrowed by name**.

That price is wrong here, and getting it wrong would be the expensive mistake this spike exists to
prevent. **Not one of the five hatches is an evaluator.** Every one of the five specifications
demands compilation in its own normative text:

| Consumer | Its own words |
|---|---|
| `visual-scripting` | *"A graph SHALL be a source representation compiled to an executable program. It SHALL NOT be the runtime object model."* |
| `gameplay-abilities-and-effects` | *"compiled into an ability program shared by every owner […] executed without graph traversal or reflection at activation time"* |
| `ai-system` | *"AI graphs SHALL be compiled, not interpreted node-by-node at runtime […] it SHALL contain compiled programs and no graph compiler"* |
| `animation-and-skinning` | *"SHALL be compiled, not interpreted node-by-node at runtime […] the runtime SHALL contain no graph compiler"* |
| `sequencing-and-cinematics` | *"Runtime SHALL evaluate the program. Editor timeline object graphs SHALL NOT be traversed at runtime."* |

The one construct that could be mistaken for an interpreter is `ai-system`'s program counter and
execution stack — and `visual-scripting` names that shape and permits it: *"a **typed register
machine** with a shared program and separate state. There SHALL NOT be one virtual machine instance
per entity."* One shared bytecode over packed per-agent state is the pattern, not the violation.
`ai-system`'s GOAP search and `animation-and-skinning`'s motion matching are likewise runtime
searches over **compiled tables and indexed data**, not over graphs.

**So the exit criterion "No graph is interpreted at runtime" holds for all seven, is narrowed for
none, and must not be narrowed in `ROADMAP.md`.** The fourth row the table needs is:

| Spend | What it buys | What it costs |
|---|---|---|
| The consumer keeps **its own IR and its own compiler**, sharing the authoring front end | that consumer compiles, through a language suited to it | one more compiler to write and to test; **nothing is interpreted, and no exit criterion is narrowed** |

Under that row the spike spends **five of the fourth kind and two extensions, and zero of the third
kind**. The instrument, not the milestone, is what §1 got wrong: its table was drawn assuming the
only alternative to one IR was an interpreter, and the specifications had already ruled out both.

### 1.6 What is actually shared — measured, not asserted

P11 is the constructive finding. `visual-scripting`'s spec already names the shared layer, and it is
not the IR:

> The infrastructure SHALL own: node and pin models, typed connections, stable identity,
> serialization, subgraphs, the editor canvas and its undo, diffing and merging, versioning and
> migration, and debugging.

Every item on that list exists in `src/rendering/material/` in domain-neutral form and was measured
there: **14 of 24 `GraphOp` enumerators carry no material semantics**, and neither do
`GraphNode::id` (stable authoring identity, separate from `NodeId`), `muted` (an author's mute that
lowers to nothing rather than being deleted), `flags` as a side table outside the identity,
`connect(from, to, port)` with last-wire-wins, `Builder::add_origin` (provenance as a **set**,
because interning merges authoring nodes), `OptimiseReport::dropped_origins` (which nodes were
dropped, by authoring id) and `GeneratedSource::value_nodes` (the debug map from a program location
back to a node).

And one thing below the front end is shared too, measured by P8 and P12: **the pure-expression SSA
core itself** — hash-consing, canonical commutative ordering, canonical visit order, constant
folding, common-subexpression elimination, dead-node elimination, the content-hashed digest and the
provenance side table. Every domain has expression islands, and P12 encoded three real ones —
two VFX kernels and a camera smoothing rig — in the IR **exactly as it stands today**, with their
per-frame state carried in as an input and out as a root.

### 1.7 The verdict the implementing agents build from

**Build one front end, one expression core, and six back ends. Not one IR.**

1. **`CyberGraph`, the shared authoring layer** — section 2's real deliverable, and the whole of
   `visual-scripting`'s "Shared infrastructure" requirement. Nodes, typed pins, typed connections,
   stable identity, layout separated from semantics, deterministic textual source, subgraphs,
   semantic diff and three-way merge, versioning and migration, opaque preservation of missing
   plugin nodes, node/pin-precise diagnostics, the debug map, capability sets and the determinism
   audit. **Every one of the seven adopts it.** This is where "one editor, one diff format, one
   debugging model" is delivered, and it is the promise the milestone can actually keep.
2. **A shared pure-expression SSA core** — a new module, generalised from `src/rendering/material/`'s
   IR by E1, E2, E3 and E4. **Do not modify `src/rendering/material/` in this milestone**: that
   module is M7's closed work, E2 invalidates its cook keys, and re-testing it mid-milestone buys
   nothing the criterion below does not buy more cheaply. Instead:
   **its criterion is P13's anchor.** The generalised core, given the material op table and type
   lattice as a domain, must reproduce the tree's own reference material at IR digest
   **`f48f3faf395e52fd`**, post-pipeline digest **`178a3630921e0506`** (16 nodes, 3 dropped, 10
   merged, 1 folded) and program digest **`7f74500626ea001a`**. A core that hits all three has lost
   nothing the material compiler depends on, and porting the material compiler onto it becomes a
   later mechanical change rather than a risk inside this milestone.
3. **Six lowerings, each with the IR its own specification names**, all compiling, none interpreting:

   | IR | Serves | Shape its spec requires |
   |---|---|---|
   | Material IR (**exists**, M7) | `material-compiler` | expression DAG → Slang |
   | **CyberGraph IR** (new) | `visual-scripting`, `gameplay-abilities-and-effects` | basic blocks, control flow, calls, events, commands, queries, suspension points → bytecode and native |
   | **VFX IR** (new, on the shared core) | `vfx-system` | SSA expression, one root per written attribute → Slang |
   | **Animation IR** (new) | `animation-and-skinning` | pose-valued blend tree plus a state machine, lazily evaluated |
   | **AI behaviour IR** (new) | `ai-system` | flat instruction stream plus a parameter table, over a shared register machine |
   | **Sequence program** (new) | `sequencing-and-cinematics` | interval index, event table, channel data, preload plan, over exact rational time |
   | **Camera rig program** (new, on the shared core) | `camera-system` | transfer-function DAG with declared roots and one phase boundary |

**Delivery scope does not change. Section 1's framing does, and section 2's target does.** Section 2
is `visual-scripting` → Working, and its four tasks are unchanged in substance; what changes is that
2.1's "shared graph infrastructure" is the whole of what is shared across seven consumers, 2.2's "the
IR, and execution backends" is `CyberGraph`'s own IR with its bytecode and native back ends serving
two consumers rather than seven, and the shared expression core is a second, smaller deliverable
beside it. Sections 5 to 8 each own their lowering rather than inheriting one, which is more code
and **less coupling**: a semantic discovered late in animation is one compiler's problem instead of
six consumers' re-test.

**2.4 gets easier, not harder.** *"No graph is interpreted at runtime — a test that fails on a
per-entity virtual tick in any consumer"* is a property of each consumer's runtime, not of a shared
IR, and all seven specifications already demand it. The test to write is one that walks each
consumer's runtime and fails on a per-entity graph object, interpreter instance, virtual update or
string-keyed node lookup — the checkable list `visual-scripting`'s "Forbidden visual scripting
patterns" already enumerates.

**One thing to reject on sight.** If a later proposal reaches for one IR again — because six
compilers looks like duplication — it is refused by `visual-scripting`'s "No universal
representation" scenario, and P3, P4, P5, P6, P7 and P10 are the measurements that say what it would
cost.

### 1.8 What this spike did not settle

- **The shared expression core's own scope is a judgement, not a measurement.** P12 shows VFX and
  camera fit it; it does not show that fitting them is cheaper than each writing its own SSA pass
  set. The anchor in 1.7 is what makes the judgement checkable either way.
- **`ml-inference` was not measured.** It is Seed in this milestone and is not one of §1's seven, but
  it will present a graph, and nothing here says whether its graph is a consumer of `CyberGraph` or
  an imported model format that only borrows the determinism boundary.
- **Nothing was measured on Windows or macOS**; this machine is Linux only. The findings are
  properties of the IR's data structure rather than of a platform, so they should hold, and that is
  a prediction rather than a result.


## 2. Order, and why the interface is not last

The dependency graph in `docs/roadmap/dependencies.md` puts `visual-scripting` above abilities, AI,
sequences, animation and VFX, and `navigation` above AI. That fixes most of the order. The one
choice not forced by dependencies is where `ui-system` and `text-and-fonts` go, and they go **early
rather than last**, because the vertical slice is the artefact and an artefact with no heads-up
interface cannot show an ability's cost, an effect's duration or a team's score. An interface built
last is an interface tested once.

## 3. What M8.a leaves for this milestone to finish, and what it warns about

**Two inherited items, both recorded at M8.a's closing gate rather than discovered here:**

- `serialization-and-prefabs`' "Apply and extract" has no implementation and has not had one since
  M2. The data model supports it; the operations are not written.
- **The mesh a created primitive references reaches no renderer.** `cy::render::MeshRenderer` is a
  name in `src/scene/src/node_template.cpp`'s catalogue with no reflected type behind it, so a world
  authored in the editor round-trips through `.cyworld` and is drawn by nothing. M8.a's own artefact
  photograph shows an authored sphere and box as two identical unit boxes through M3's fixed slots.
  That is `editor-viewport-and-gizmos` and the renderer's, both of which complete here.

**And the warning M7's gate wrote for M8 and M8.a did not have to face.** Of the modules under
`src/rendering/`, `cy_rendering_forward`, `cy_rendering_material`, `cy_rendering_post`,
`cy_rendering_shadows`, `cy_rendering_sky`, `cy_rendering_temporal`, `cy_rendering_gpu_culling` and
`cy_rendering_virtual_texturing` are linked by **nothing but their own test binaries**. Nothing in
the tree assembles a frame out of the renderer's own parts. A vertical slice is the first artefact
that cannot avoid it, and `vfx-system`, `rendering-2d` and `ui-system` all render into that frame.
**Assembling the renderer is a prerequisite of this milestone's artefact and it has no owner on the
ladder.** It is stated here so that it is planned rather than discovered in section 12.

## 4. What must not be retrofitted

| Invariant | Why it cannot wait |
|---|---|
| One **front end**, adopted by all seven | Seven graph editors with seven diff formats and seven debuggers is the failure `visual-scripting`'s shared-infrastructure requirement was written against. **Superseded §1's "one IR" invariant at the spike (§1.7): six lowerings are the plan, one authoring layer is the invariant** |
| A domain's IR is extended deliberately, never patched | A lowering that grows an op for one caller becomes two dialects with one name. E1-E4 (§1.4) are the shape an extension is specified in |
| No graph interpreted at runtime | A per-entity virtual tick in one consumer becomes the pattern the next five copy |
| The determinism firewall | VFX and inference must not write gameplay state; a firewall added after the fact is a firewall with holes |
| Cost bounded by configuration | 8,000 agents and 100 concurrent effects are budgets, and a budget discovered in a frame is not one |

## 5. The spike itself

It lives **outside the repository**, as M3's, M5.5's, M6's and M7's two did: a prototype under
`docs/` fails `just quality-layers`, correctly, and every previous spike was more useful for being
disposable.

| | Path | Re-run |
|---|---|---|
| 1.1–1.3 the shared graph IR | `~/cyberdyne-spikes/m8b-ir-spike` | `bash run.sh` |

`run.sh` compiles the probe **together with the engine's material compiler from source** — not
against a prebuilt library — under `clang++` and `g++`, at `-O0` with ASan and UBSan and at `-O2`,
and runs all four. The four runs must print byte-identical text, because every figure §1.3 quotes is
a property of the IR and not of the compiler that built it; `run.sh` diffs them and fails if they
disagree. It takes about two minutes, most of it the four compilations.

**The probe records gaps and derives its exit status from them**, honouring
`samples/harness/artefact.py`'s first rule in a program that is not an artefact: six gaps, so it
exits 1. `run.sh` itself fails only on a broken build, a crash, or a disagreement between the four —
a probe exiting 1 with six gaps is the expected result and the whole finding.

