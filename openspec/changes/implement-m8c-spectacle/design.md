# Design: M8.c — Spectacle

## 1. What M8.b's spike already decided for this milestone

**There is no new spike here, and that is a finding rather than an omission.** M8.b's spike asked
one question seven times — can a single graph IR express what each consumer needs — and two of the
seven it asked about are this milestone's: `vfx-system` and `sequencing-and-cinematics`. Its answer
covers them, it is committed at `~/cyberdyne-spikes/m8b-ir-spike` and recorded in
`openspec/changes/implement-m8b-systems/design.md` §1, and re-deriving it would spend a milestone's
risk budget on a question that already has a measured answer.

What that answer means here, concretely:

| Layer | Shared? | Why |
|---|---|---|
| **CyberGraph** — nodes, typed pins, stable identity, semantic diff and merge, migration, opaque preservation | **Yes, adopted unchanged** | It is an authoring layer, not an execution model. Every M8.b consumer adopted it and these two do the same |
| **The pure-expression SSA core** (`src/graph/expr.h`) | **Where the sub-language is pure** | A particle's per-attribute expression is pure and belongs on it. A timeline is not |
| **The execution form** | **No — one per consumer** | A particle kernel is a data-parallel program over a compiler-derived attribute layout; a timeline is an ordered dispatch against exact time. Neither is the other, and neither is a register machine |

**The five blockers the spike measured apply here exactly as they did there.** The expression core
is a hash-consed DAG whose identity is a content hash: no back edges, no writes, commutative
operands re-sorted, values nothing reads deleted, both arms of a select evaluated. A timeline track
is an authored order — blocker three by itself. A particle kernel writes attributes — blocker two.
So both consumers keep their own IR, which is what their own specifications already say:
`vfx-system` requires "graph compilation to an intermediate representation" of its own and
"compiler-derived attribute layout", and `sequencing-and-cinematics` requires "compiled programs"
with "batched subsystem dispatch".

## 2. The firewall, and why it is section 1 of the tasks

`vfx-system` and `ml-inference` each state the same rule from their own side and **neither states
where it is enforced**:

> VFX ... SHALL NOT be a source of truth for: damage, hit detection, entity creation or destruction,
> physics state, network-replicated values, or any value consumed by deterministic simulation.

> Model output SHALL NOT drive authoritative gameplay state ... unless the session is **pinned**.

A rule stated on the producer's side and enforced nowhere is a convention. Two candidates for the
enforcement point exist today because M8.b built them:

1. **`gameplay-framework`'s command origin.** Sequences already have to be indistinguishable at the
   simulation's input; the same seam can make a VFX-originated command distinguishable *to the
   check* while remaining indistinguishable *to the simulation*.
2. **The ECS write path.** A development-build write barrier that knows which system is writing is
   what "Development builds SHALL detect and report" asks for in as many words.

**Deciding between them is task 1.1 and it precedes any VFX code**, because a firewall retrofitted
after two systems already write through it is a rewrite rather than an addition. The test in task
1.4 carries a negative control for the same reason M8.b's `slice-control` does: a firewall test
that still passes when the firewall is deleted is a test of nothing, and this project has shipped
four criteria that never ran.

## 3. What this milestone deliberately does not do

- **No fluids.** `vfx-system` defers them with reserved seams and this milestone keeps that
  deferral; the seams are checked, not filled.
- **No second slice.** The artefact is `samples/08-vertical-slice` extended. A second sample would
  demonstrate these systems working beside a copy of the game rather than inside it, which is the
  weaker claim and the one that hides integration cost.
- **No inference backend beyond Seed.** `ml-inference` reaches Seed: the abstraction, the asset, the
  session and the boundary. A real backend is a dependency decision, and `thirdparty-dependencies`
  wants it justified against a shipped game rather than against a demo.

## 4. Where the risk actually is

Not in the compilers — that is the point of §1. It is in the GPU half of `vfx-system`: simulation on
a device queue, GPU-to-GPU events, and a readback path that must be bounded rather than merely
present. That work is bounded by seams that already exist and are already gated — the render graph
declares reads and writes and derives its own barriers, async compute is a declared queue, and
`-DCY_RENDERER_VULKAN=OFF` is a configuration this project now builds rather than assumes. The
honest statement of the risk is that the GPU path is the half with unknowns and the CPU path is the
half that must therefore be a *declared* fallback that reports itself, never a silent one.
