# Design: M7 — Fidelity

This document is opened with the milestone and is filled in by the spikes before the implementing
work starts. What is written here now is the shape of each decision and the criterion that settles
it, so that a spike is commissioned against a question rather than a subject.

**Sections 1 and 2 are closed.** Both spikes were run, both met their criterion, and what they found
is recorded below so the implementing agents do not re-derive it. Section 6 says where they live and
how to re-run them.

## 1. Spike — the material IR and closure lowering

**The question.** What is the intermediate representation between a material graph and a compiled
program, and what does lowering to closures cost when the same material has to appear in the forward
pass, the visibility-buffer material resolve, a shadow pass, a GI probe and a node preview?

**Why it is expensive to reverse.** Every other system in this milestone consumes the material table.
An IR that cannot express what the visibility buffer needs is discovered when the visibility buffer
is written, which is after virtual geometry has been built on top of it.

**The criterion**, and it is one of M7's own exit criteria so the spike tests the gate rather than a
proxy: the IR round-trips, and a graph and a hand-written material produce **identical programs**.

### 1.1 The result

`~/cyberdyne-spikes/m7-material-spike` — **37 checks, 0 failures**, identical under `clang++ 18.1.3`
and `g++ 13.3.0`, at `-O0` with ASan and UBSan and at `-O2`. The criterion is met: a node graph and a
hand-written text definition of the same material produce IR digest `296c7bf71ed43cb9` and program
digest `39caab8f7a60ab78`, and the generated Slang is byte-identical. Both digests are the same under
all four builds, which matters because the digest is a cook key.

The two front-ends were deliberately written to be as unalike as two real authoring paths are. The
graph emits what an editor emits: a weight port on every closure, a constant node feeding a multiply,
one texture sampled from two separate nodes, an operand order chosen by wiring, a muted emission
closure the author did not delete, and a node left disconnected. The text emits what a person writes:
a `let`, a literal, no redundant weight. They are equal only *after* the compiler runs.

### 1.2 What makes the two paths one — the load-bearing decisions

1. **Identity is a content hash computed bottom-up over the DAG, and nothing that is not the
   material's meaning is allowed into it.** The hash closes over the op, the result type, the symbol
   **name**, the immediates, and the operands' content hashes. It closes over nothing else — not the
   node id, not the order the front-end called the builder, not which front-end it was.
2. **The symbol *name*, never the symbol-table index.** Two front-ends intern names in different
   orders. A hash over indices makes two identical materials differ.
3. **Commutative operand lists are canonically ordered by that same content hash** — never by node
   id, which is construction order wearing a disguise. The graph wires `scale * texture` and the text
   writes `texture * scale`; only this makes them one value.
4. **Provenance is a side table, never a node field.** Two authoring nodes routinely produce one
   value once CSE has run, so attribution is a *set* attached to a node id, and putting an origin
   into the identity would fork the value and defeat the criterion. The spike asserts that
   attribution does not change the program.
5. **The builder is the only way to make a node.** Typing, canonicalisation and interning all happen
   there, so a front-end cannot construct something the other front-end could not reproduce.
6. **Emission numbers SSA values by canonical visit order.** Numbering them by node id leaks
   construction order into the generated source; the spike proves this with a module whose content is
   identical and whose ids are shifted (program `a8bfeafa4edcea00` against `39caab8f7a60ab78`).
7. **One spelling per operation.** `1 - x` as a `OneMinus` node in one front-end and as `Sub(1, x)`
   in the other would never unify. The builder must offer one form and the front-ends must use it.

### 1.3 The pass pipeline is a rebuild, and it must reach a fixed point

Every optimisation pass is expressed as a bottom-up **rebuild through the same builder**. That is not
a shortcut: it is what guarantees a pass's output is in the same canonical form as its input, so the
pipeline can be iterated to a fixed point and the fixed point is reachable from either front-end. The
24-node test material reaches it in **3 iterations**.

**Dead-node elimination is a property of the rebuild, not a pass.** A rebuild that starts at the
outputs cannot reach an orphan. The spike's first run found its own `pass_dce` switch to be a
placebo for exactly this reason — turning it off changed nothing, because the rebuild had already
done the work. A disable switch, which `material-compiler` requires so a miscompilation can be
bisected, has to carry the orphans across explicitly.

### 1.4 Which passes are part of the language, and which are optimisations

Each row turns one thing off and re-asks the criterion. A row that still produces identical programs
is an optimisation; a row that does not is part of the material language, and disabling it in a
development build changes what the material *means*.

| Switched off | IR digest | Program | Verdict |
|---|---|---|---|
| interning (hash-consing) | same | **differs** | load-bearing |
| canonical commutative order | **differs** | **differs** | load-bearing |
| canonical emission order | same | **differs** | load-bearing |
| constant folding | **differs** | **differs** | load-bearing |
| common subexpression elimination | same | **differs** | load-bearing |
| texture sample deduplication | same | **differs** | load-bearing |
| dead-node elimination | **differs** | **differs** | load-bearing |
| closure simplification | **differs** | **differs** | load-bearing |
| uniform/varying analysis | same | same | optimisation only |

The consequence for `material-compiler`'s "Each pass SHALL be individually disableable in development
builds": eight of the nine switches change the compiled program, so a bisection build is not a build
that produces the same material more slowly. That has to be said in the diagnostic when a pass is
disabled.

### 1.5 Lowering, the program family and cost

- **Closure-set matching** against known shading models works on the *set of leaf closure kinds*
  reachable through scale/add/layer. `{diffuse, specular}` and `{diffuse, specular, emission}` lower
  to `StandardMetallicRoughness`; adding `coat` gives `ClearCoat`; anything else takes the generic
  layered evaluator and the cook report says so. The spike's layered car paint (coat over diffuse +
  specular + sheen) correctly takes the generic path.
- **Derivation must drop only *leaf* closures, never the combinators.** Dropping a `CScale`, `CAdd`
  or `CLayer` deletes everything beneath it. The spike's first far-field program came out with zero
  closures for this reason.
- **"A wrong derivation is visible" is a reachability question, not a numeric one.**
  `material-compiler` asks the cook report to flag a derivation that changes a material's average
  albedo. A numeric fold gives up the moment a runtime parameter is in the path — the spike's worn
  metal multiplies albedo by `1 - metallic` and never folds. Computing instead **which textures reach
  a diffuse or specular colour input**, and comparing that set between the primary and the derived
  program, answers the question for every material: worn metal is `{base_color} | {base_color}` (not
  flagged), and a material whose microdetail `grime` texture also feeds albedo is
  `{base_color, grime} | {base_color}` (flagged).
- **An opaque material has no shadow program at all.** When the opacity output folds to a constant 1,
  the derived module carries no opacity output and the emitter has nothing to emit — which is
  `material-compiler`'s "Opaque materials SHALL produce no fragment work in the shadow program" as a
  structural fact rather than an empty function.
- **Opacity is a second root.** It must survive dead-node elimination and be part of the IR identity,
  or the shadow program is built from something the digest did not cover.
- **Node previews are the same emitter rooted at a different node.** Preview of the surface root
  emits the primary program's body byte for byte; an interior node's preview is a subsequence of the
  final program's statements. There is no second code generator to keep in step.

### 1.6 What it costs

Graph → optimised IR → generated Slang for the 24-node material, `-O2`, 400 iterations per run:

| Run | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| median | 56.5 us | 56.5 us | 57.6 us | 56.8 us | 58.6 us |
| max | 75.2 us | 162.9 us | 197.4 us | 224.6 us | 111.8 us |

The median moves 2.1 us across five runs and the max moves 149 us. **Report the median.** This is the
same lesson M6's open-world artefact learned the expensive way, reproduced here on a machine with a
verified-quiet process tree.

### 1.7 What the implementing agent should build first

The IR, its content hash, the builder, the serialiser and the round-trip test — before any front-end.
The criterion is a property of that layer and of nothing above it, and it is cheap to hold from the
start and expensive to retrofit. The spike's `ir.h` is 224 lines and its `ir.cpp` 712; the whole
prototype including two front-ends, lowering, the program family and the checks is 2,396 lines. The
arbiter spike is 1,094.

## 2. Spike — the budget arbiter's control loop

**The question.** Six systems each declare a cost and a set of quality levers; the arbiter allocates
frame time between them every frame. What control law converges without oscillating, and what does a
system have to declare for the arbiter to be able to reason about it?

**Why it is expensive to reverse.** `residency`'s lever schedule at M6 is the same shape a level
lower — declared levers, tightening immediately and relaxing after a dwell — and it was designed that
way so that this arbiter is one mechanism rather than seven. If the arbiter needs something a lever
cannot declare, every system that has already declared one is edited.

**The criterion**: the arbiter's allocations converge without oscillation under a **step load**, and
every paged system degrades along its declared axis — a coarse root, a resident mip tail, a
stale-but-valid shadow page. A frame is never missing, only coarser.

### 2.1 The result

`~/cyberdyne-spikes/m7-arbiter-spike` — **22 checks, 0 failures**, byte-identical output under
`clang++` and `g++` at `-O0`+ASan/UBSan and `-O2`. The criterion is met over a sweep, not a run.

**Why a sweep.** The levers are a discrete ladder, so whether a control law oscillates depends on
where its equilibrium happens to land relative to a ladder boundary. **One step magnitude can make
any law look stable** — the spike's own first run certified a law at one load that oscillated on 27
of 71. Every experiment is therefore 71 step magnitudes from x1.00 to x2.40, each held 700 frames and
released, and reports how many of them ended with *any* lever change in the last 450 frames of the
plateau. A settled loop moves zero times.

Model: 7 subsystems with 3- or 4-position declared ladders, a 13.90 ms budget of which 1.20 ms is not
allocatable, a nominal scene costing 12.15 ms at authored quality, +/-3% per-frame measurement noise,
2 frames of readback latency.

### 2.2 The recommended loop

| Element | Value | Why |
|---|---|---|
| Arbiter period | 8 frames | The spec's "longer time constant"; the controllers run every frame |
| Actuator | **the declared reduction order, one step per subsystem per tick** — never a uniform scale | see 2.3 |
| Gain | 0.35 (cover 35% of the deficit per tick) | anything from 0.20 to 2.00 is stable at 1–8 frames of latency |
| Deadband | **0.5 x the coarsest reachable lever quantum** | 2.5x the smallest multiple that holds the sweep still; see 2.4 |
| Setpoint | budget **minus** the deadband | puts the band's upper edge on the budget, so a settled frame is never over it |
| Restore | at most **one** step per tick, reverse declared order, only when `measured headroom − deadband` covers the step's predicted increase | see 2.5 |
| Relax permission | **arbiter only** — a controller may tighten on its own, never relax | see 2.6 |
| Controller tighten test | `measured > allocation * 1.06` | the margin must exceed the noise that survives the filter; see 2.7 |
| Controller relax test | `predicted(next better) <= allocation * 0.88` | the two margins straddle the allocation |
| Forced allocation | target position's cost x 1.08 | same reason as the tighten margin |
| Filter | EMA alpha 0.25, on the frame and on each subsystem's cost | |
| Controller relax dwell | 6 frames; the arbiter will not grant a step back within 18 | relaxing answers a frame that is fine; there is no hurry |
| Resolution scale | last lever, only when every subsystem is at its minimum | it multiplies every raster-bound subsystem, so it is not a peer allocation |

Result: **0 of 71 loads oscillate**, none settles over budget, settling is 56 frames median and 112
at p90 (about 1–2 s at 60 Hz), and the loop leaves 1.21 ms of the 12.70 ms allocatable budget unspent
— the quantisation floor of a ladder this coarse.

### 2.3 The single most important finding: the actuator is the declared order

**A uniform scale over every allocation is the wrong actuator.** It moves whichever subsystems happen
to sit nearest a ladder boundary, so one tick drops four of them at once and the next tick gives them
all back. `residency::plan_reduction` already walks `SubsystemPolicy::reduction_order` and moves
levers in that order; the arbiter must do the same thing with frame time.

Concretely: when the frame is over budget by `e`, walk the subsystems in ascending `reduction_order`
and force **one** step down from each — by setting its allocation just under what its current
position costs — until `gain * e` of the deficit is covered. Nothing is scaled. `reduction_order`
keeps `residency`'s meaning (lower reduces first), so post-processing and VFX go before geometry and
material, and restoration walks the same list backwards.

Replacing the uniform scale with this took the sweep from 27 of 71 loads oscillating (worst 411 lever
changes) to 2 of 71 (worst 10), before the remaining fixes in 2.6 and 2.7 took it to zero.

### 2.4 No single mechanism is sufficient, and the deadband is the one that finishes the job

Factorial over the four candidate mechanisms, gain 0.5, 2 frames of latency, 71 loads each:

| Control law | oscillating loads | worst | settle p90 | wasted |
|---|---|---|---|---|
| raw, no dwell, no deadband, arbiter every frame | 64/71 | 900 | 699 | 1.54 ms |
| EMA filter only | 63/71 | 709 | 699 | 1.10 ms |
| relax dwell only | 59/71 | 270 | 699 | 0.94 ms |
| deadband only | 56/71 | 900 | 699 | 3.88 ms |
| arbiter period 8 only | 63/71 | 56 | 696 | 0.14 ms |
| EMA + dwell | 44/71 | 279 | 699 | 1.80 ms |
| EMA + dwell + period 8 | 38/71 | 24 | 688 | 0.15 ms |
| EMA + dwell + deadband, period 1 | 5/71 | 260 | 70 | 1.47 ms |
| **all four** | **0/71** | **0** | 96 | 2.24 ms |

The deadband's size, against the coarsest single-lever cost quantum reachable from the current state:

| multiple | oscillating | worst | settle p90 | wasted |
|---|---|---|---|---|
| 0.00 | 38/71 | 24 | 688 | 0.15 ms |
| 0.05 | 12/71 | 9 | 368 | 0.19 ms |
| 0.10 | 5/71 | 2 | 152 | 0.24 ms |
| 0.15 | 3/71 | 1 | 136 | 0.35 ms |
| **0.20** | **0/71** | 0 | 112 | 0.44 ms |
| 0.25 | 0/71 | 0 | 112 | 0.61 ms |
| 0.50 (recommended) | 0/71 | 0 | 112 | 1.21 ms |
| 1.00 | 0/71 | 0 | 96 | 2.24 ms |
| 3.00 | 0/71 | 0 | 56 | 5.13 ms |

The edge is at 0.20; 0.5 is recommended for the safety factor, and the curve is the argument for not
going to 1.0, which costs 2.24 ms of a 12.70 ms budget for nothing the sweep can measure. Rows 0.75,
1.50 and 2.00 each show a single lever change on a single load — one change in 450 frames is not a
limit cycle, but it is not zero either, and it is why the recommendation is read off a curve rather
than off one row.

**The deadband has a price and it must be paid deliberately.** A deadband centred *on* the budget is
by construction a refusal to correct an error smaller than one lever quantum, so the loop settles
happily **over** the budget. Setting the setpoint one deadband below puts the band's upper edge on
the budget instead. Measured at the recommended 0.5 multiple, over the same 71 loads:

| setpoint | oscillating | loads settling over budget | worst | median wasted |
|---|---|---|---|---|
| on the budget | 1/71 | 2 | +0.22 ms | 0.61 ms |
| **one deadband below** | **0/71** | **0** | +0.00 ms | 1.21 ms |

The price of never being over budget is 0.60 ms of a 12.70 ms allocatable budget. Pay it: M7's exit
criterion is that the scene *holds* its frame budget, and a loop that settles over it by less than a
lever quantum is still a loop that settles over it.

### 2.5 Restoring quality is not the inverse of taking it away

Four separate defects, all found in the spike's own drafts, all in the restore path:

1. **Cap an allocation at what the subsystem can spend at its best declared position.** Without a cap
   every under-budget frame scales the allocations up again, nothing changes because everything is
   already at position 0, and the next spike is absorbed by accumulated slack instead of by the
   levers. This produced a 54-frame relaxation limit cycle that looked like a control-law defect.
2. **Measure the surplus; do not book-keep it.** Headroom is `budget − measured frame time`, not
   `allocatable − sum of allocations`. The second is wrong by exactly the amount the ladder wastes,
   so restoring against it grants steps the frame cannot pay for.
3. **Grant all-or-nothing.** A partial grant that cannot buy the step is the worst of both: it spends
   the surplus and changes nothing, and the arbiter then creeps the level up until the step fires
   between two arbiter ticks, at a moment nothing chose.
4. **A relaxation must leave the frame at least one deadband inside the budget.** Tying the relax
   threshold to the deadband, rather than to a separate safety factor, is what makes the two branches
   stop handing the frame back and forth.

### 2.6 Tightening is local; relaxing is arbitrated

`rendering-architecture` forbids a subsystem controller to measure total frame time. The spike found
that the rule has a second half nobody writes down: **a controller must not relax on its own
authority either.**

A subsystem that has overrun its own allocation must fix that now and needs nobody's permission — its
cost is its own. Relaxing is not symmetric with it: the time a step back up costs comes out of the
frame, and the frame is precisely the quantity the controller is forbidden to see. A controller that
relaxes on its own is spending a budget it cannot see. Making relaxation require an explicit
permission set by the arbiter when it grants a step was worth 10 lever changes per 450 frames.

The forbidden case is modelled and measured. With every controller running its own copy of the
arbiter's decision on the same global signal — seven corrections for one error, which is exactly the
spec's "the VFX and post-processing controllers SHALL NOT independently reduce quality for a cost
they did not incur" — **57 of 71 loads oscillate, worst 659 changes**, against 0 for the recommended
loop.

### 2.7 The ratchet, and why the two margins must straddle the allocation

Because tightening is unconditional and relaxing is arbitrated, a controller whose tighten test has
no margin loses a step to every noise excursion and never gets it back. It does not look like
oscillation at first; it looks like a scene that mysteriously gets coarser the longer you stand
still. Oscillating loads out of 71:

| measurement noise | margin 0 | 0.02 | 0.04 | 0.06 | 0.10 | 0.15 | margin 0.06, EMA 0.10 |
|---|---|---|---|---|---|---|---|
| +/-1% | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| +/-3% | 2 | 2 | 1 | **0** | 1 | 0 | 1 |
| +/-5% | 4 | 3 | 2 | 3 | 2 | 3 | 1 |
| +/-8% | 5 | 8 | 8 | 10 | 12 | 13 | **2** |

At the +/-3% this model assumes, a 6% margin — twice the noise — removes it. The single-load
differences along that row are not worth over-reading. The +/-8% row is: it is monotone and it says
that **at high measurement noise a wider margin makes things worse, because it delays the response
until the arbiter has already overshot.** The answer there is a heavier filter (EMA 0.10 takes 8%
noise from 5 loads to 2), not a wider margin. An engine that finds its GPU timings this noisy should
filter harder, not deaden the controller.

### 2.8 What it costs to sample

**Latency costs nothing in the range an engine actually has.** Oscillating loads out of 29, by
readback latency and gain, deadband on:

| delay (frames) | gain 0.20 | 0.35 | 0.50 | 1.00 | 2.00 | deadband off |
|---|---|---|---|---|---|---|
| 0 | 1 | 1 | 1 | 1 | 1 | 11 |
| 1 | 0 | 0 | 0 | 0 | 0 | 8 |
| 2 | 0 | 0 | 0 | 0 | 0 | 10 |
| 3 | 0 | 0 | 0 | 0 | 0 | 16 |
| 4 | 0 | 0 | 0 | 0 | 0 | 11 |
| 8 | 0 | 0 | 0 | 0 | 0 | 22 |
| 16 | 10 | 12 | 14 | 19 | 17 | 23 |

One to eight frames of GPU-timestamp latency costs nothing at any gain from 0.20 to 2.00. At sixteen
frames the loop is unstable and raising the gain makes it worse. **It is the deadband and not the
gain that is doing the work**: with the deadband off there is no stable gain at any latency. The
practical consequence is that the implementing agent does **not** have to tune a gain against the
readback depth, which is the tuning exercise this spike was expected to produce and did not.

**CPU cost is negligible.** One arbiter step, median of 20,000 samples: 33 ns at 4 subsystems, **42 ns
at 8**, 59 ns at 16, 107 ns at 32, 226 ns at 64 — once every 8 frames.

### 2.9 The degradation criterion

- **Starve everything at once (x4.0 load):** all 7 subsystems reach their last ladder position and
  report that they are at their minimum, resolution scale falls to 0.82, the tail median is 13.41 ms,
  **0 lever changes** — the loop is still there too, at the bottom of every ladder. Nothing
  disappeared and no subsystem was allocated below its reserved minimum. A frame is coarser, never
  missing: every ladder's last position has a non-zero quality by construction and the spike asserts
  it.
- **Pinned mode is total:** the arbiter and every controller stop together, quality stays at 1.00,
  and 700 frames of overrun are *reported* rather than corrected (worst +7.42 ms).
- **Release:** authored quality returns on 29 of 29 loads, none with a long tail of adjustments.

### 2.10 What `residency`'s declared lever shape needs added — exactly one thing

Everything the arbiter needs already exists in
`src/servers/residency/include/cy/servers/residency/policy.h`: a discrete ladder of declared
positions (`LeverSchedule`), a `declared` flag so a subsystem without a lever is not pretended to
have one, a declared `reduction_order`, asymmetric hysteresis with a relax dwell, and a pinned mode
that stops everything at once.

The one addition: **a subsystem must declare what each ladder position costs relative to position 0.**
An arbiter allocating milliseconds over a ladder it cannot price is choosing blind, and every
mechanism in 2.2 — the deadband derived from the coarsest quantum, the one-step forcing, the
all-or-nothing grant — is expressed in terms of that number. `LeverSchedule` has three declared
values already; this is a fourth field beside them, not a new mechanism.

**Also, and this is a modelling trap rather than a design one:** the nominal scene must fit the
budget with headroom. The spike's first model had a baseline costing 17.1 ms against a 13.9 ms budget
and produced a limit cycle that looked like a control-law defect and was a content defect. When
`samples/07-fidelity` is authored, its nominal state must be inside the budget or the arbiter will be
blamed for the scene.

## 3. Virtual geometry's cluster hierarchy and GPU traversal

Third in the roadmap's order and not given a separate spike, because the two above decide what it can
assume. What must be settled before it is built: the cook is deterministic and cache-friendly at
cluster granularity, per `asset-import-pipeline`'s "Virtual geometry cooking" requirement — which is
the requirement that stops `asset-import-pipeline` completing at M6 — and it lands as a **node in
the build graph** rather than beside it.

Two things section 1 settles for it. Its attribute decoder is a front-end to the same IR, so a
compressed cluster attribute reaches a material through the semantic interface and not through a
second path. And its cook, like the material cook, is keyed by a content hash whose inputs are
enumerated — see section 1.2, and note that the material digest was identical under both compilers
and both optimisation levels, which is the property a shared cook cache depends on.

## 4. What M6 left that must be closed here rather than later

The proposal lists these; the two with a deadline are here.

**One key, one cache.** `tools/build/`'s key refuses an incomplete toolchain fingerprint;
`cy::import::import_derivation_key` contributes no toolchain at all, and it is the one that cooks.
M7 adds material and geometry cooks that are expensive enough that a wrong cache hit is a wrong
shipped artefact rather than a slow build. Merging them is cheapest before those cooks exist.

**One overlay, one persistent identity.** `cy::world::PersistenceOverlay` and `cy::save::Overlay`
are two structures for one requirement. Reconciling them after a save format has shipped is a
migration; before, it is an edit.

## 5. What must not be retrofitted

| Invariant | Why it cannot wait |
|---|---|
| One temporal framework, not one per stochastic system | Five reprojections that disagree about history invalidation is the defect that cannot be found from a screenshot |
| The arbiter reads declared levers, not measured guesses | A system the arbiter cannot reason about is a system that takes the frame |
| Material identity is a content hash over a canonicalised DAG | Two front-ends that are equal by luck are two implementations, and node previews become a second renderer — see section 1.2 |
| The arbiter's actuator is the declared reduction order, never a uniform scale | A uniform scale moves whichever subsystems sit nearest a boundary; see section 2.3 |
| A control law is certified over a sweep of loads, never one load | The levers are discrete, so one step magnitude can make any law look stable; see section 2.1 |
| Every new cook is a graph node with a two-run determinism gate | M6's spike measured what a non-deterministic step plus a cache costs: the artefact that ships is decided by a race |
| The Metal seed lands with the renderer, not after it | Its whole purpose is to expose Vulkan-specific assumptions while they are still cheap |

## 6. The spikes themselves

Both live **outside the repository**, as M3's, M5.5's and M6's did: a prototype under `docs/` fails
`just quality-layers`, correctly, and all three previous spikes were more useful for being
disposable.

| | Path | Re-run |
|---|---|---|
| 0.1 material IR | `~/cyberdyne-spikes/m7-material-spike` | `bash run.sh` |
| 0.2 budget arbiter | `~/cyberdyne-spikes/m7-arbiter-spike` | `bash run.sh` |

`run.sh` builds each spike under `clang++` and `g++`, at `-O0` with ASan and UBSan and at `-O2`, and
runs all four. Every check either passes or the process exits non-zero: **neither spike has a state
that reports a gap and returns 0.** The material spike takes about a second; the arbiter spike runs
roughly 25,000 simulations of 1,200 frames and takes about ten.
