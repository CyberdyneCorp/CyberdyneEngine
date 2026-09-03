# Design: the delivery roadmap

## The problem the roadmap actually solves

The specifications answer "what" and "why" for 73 capabilities. They do not answer "in what order",
and for this project that question has an unusually large blast radius, for three reasons.

**Invariants propagate.** Most of the interesting commitments in these specifications are not
features of a subsystem; they are properties of every line of code written after that subsystem
exists. "Barriers are computed, not written" is not a render-graph feature — it is a rule the
thirtieth render pass obeys because the first one did. "Transactions are the only write path" is
not an undo feature — it is why autosave, crash recovery, three-way merge and live editing are one
mechanism. Establishing such a rule costs nearly nothing at the right moment and requires
revisiting everything downstream at the wrong one.

**The dependency graph is deep, not wide.** An engine is not a set of parallel workstreams. The
renderer needs the job system; the job system's deterministic mode needs the simulation clock; the
clock needs the loop; the loop needs the ECS; the ECS needs chunk storage and reflection. Ordering
errors here do not cost a sprint — they cost the work built on the wrong foundation.

**The failure modes are known and specific.** Greenfield engines with large specified scope fail
horizontally (twelve subsystems at 80%, nothing runs) or by drift (the implementation quietly
settles a question the specification had decided). Both are addressable structurally: vertical
slices with executable gates for the first, a machine-checked status record for the second.

So the roadmap is written as a specification with `SHALL` requirements rather than as a plan
document. A plan document goes stale silently. A specification is validated, reviewed, and changed
deliberately.

## Why there are no dates

Every date in a roadmap is a compound estimate — of scope, of skill, of availability, of
discoveries not yet made — and it decays from the moment it is written. Worse, once written it
becomes the thing people track, which converts every discovery into a schedule problem rather than
a design one.

Ordering does not decay. That the ABI must be versioned from its first symbol, that Metal must be
seeded before M7's features can accidentally become Vulkan-specific, that terrain must implement a
cell payload contract that already exists — these remain true whatever the calendar does.

The cost of this decision is real: the roadmap cannot answer "when". It answers "after what", which
is the question that changes decisions.

## Why maturity tiers

A binary "implemented" flag is wrong for nearly every capability here. `core-jobs-and-concurrency`
is needed by the renderer in M3, but its deterministic scheduling mode is not needed until M9 and
its critical-path diagnostics not until profiling matters. Modelling that as one boolean forces a
false choice between blocking M3 on work nothing needs yet, and claiming a capability is finished
when it is not.

Three working tiers — Seed, Working, Complete — with the rule that a capability seeds at the
milestone its **first dependent** needs it, splits capabilities along the axis that actually
matters: the contract versus the implementation. It also makes the dependency rule expressible and
checkable: Working requires prerequisites at Seed, Complete requires them at Working.

The rejected alternative was percentage completion. Percentages of a requirement set are
meaningless when requirements differ in size by two orders of magnitude, and they invite the
horizontal failure mode directly.

## Why vertical slices, and why the artefact is committed

A milestone defined as "the renderer is done" cannot be verified, cannot be demonstrated, and
conceals integration cost until the point where it is most expensive. A milestone defined as "a
lit, textured, shadowed scene renders and matches a golden image" is verifiable by running one
recipe.

Committing the artefact matters more than the demonstration. A sample that lives in the repository
and runs in continuous integration becomes a regression gate for everything that follows. This is
why the ladder's closing artefacts are deliberately cumulative: the M4 character controller still
runs at M11, which is what makes "a later milestone may not break an earlier gate" enforceable
rather than aspirational.

## How the ladder was ordered

Four eras, twelve milestones.

**Foundation (M0–M2)** — toolchain, core substrate, world model. None of it is separately useful,
which is why the roadmap says so explicitly rather than pretending M1 is a deliverable. The
identity manifest and the commit boundary land here because everything after them encodes their
consequences.

**First playable (M3–M5)** — a frame, a scripting boundary, an editor. The order within the era is
forced: the renderer needs no scripting, but scripting needs something to look at, and the editor
needs both an ABI to talk over and a viewport to show. M4 before M5 also means the ABI is exercised
by gameplay before the editor SDK is generated from it, so ABI mistakes surface against the simpler
consumer.

**Production scale (M6–M8)** — the build graph and streaming, then fidelity, then game systems. The
build graph precedes streaming because cooked cells are derivations; streaming precedes fidelity
because virtual texturing and virtual geometry are paging systems that need the residency policy;
fidelity precedes game systems because animation, VFX and UI all publish into a GPU scene whose
shape M7 settles.

**Shipping (M9–M11)** — integrity, worlds, reach. Determinism *hooks* land in M2, but the validator
and the network built on them land at M9, because a validator with nothing to validate finds
nothing. Environment and procedural generation land at M10 as consumers of contracts M6 and M7
established. M11 is breadth: two more backends, the porting surface, and the documentation gate.

The three placements most likely to be questioned:

- **Networking at M9, not earlier.** Replication schemas depend on stable field identity (M1),
  component storage (M2), the command stream (M4), world partition cells (M6), and rollback
  primitives that are the same mechanism as replay. Building it earlier means building it twice.
- **UI at M8, not with the editor at M5.** `ui-system` states plainly that the runtime UI system is
  not the editor's toolkit. The editor is a Rust application with its own interface stack, so it
  creates no pull for the runtime UI system at all.
- **Environment at M10, after game systems.** Terrain, foliage, water and weather are the largest
  block of work whose absence blocks nothing. They consume the field substrate, the streaming
  contracts, the material compiler and the GPU scene — all of which are settled by M8.

## Cycles, and how they are broken

Three genuine cycles exist in the specification set. Each is broken the same way: the contract
seeds early, the implementation lands late.

| Cycle | Break |
|---|---|
| World partition needs terrain payloads; terrain needs cell streaming | The cell payload contract seeds at M6; terrain implements it as a producer at M10 |
| GI needs a sky; the physical atmosphere needs the GI scene's far field | An analytic sky seeds at M7; the physical atmosphere model lands at M10 |
| AI needs navigation; navigation streaming needs world partition; world partition wants AI density | The navigation interface seeds at M8 against M6's streaming; density feedback is deferred |

## Where the risk concentrates

Not in the largest capabilities — in the ones whose approach is least settled by the
specifications:

1. **The material compiler's IR and closure lowering (M7).** Every graph consumer inherits its
   mistakes, and the specifications commit to graphs compiling rather than interpreting.
2. **The renderer budget arbiter (M7).** A control loop with one measurer and many allocations is
   specified precisely because the naive version oscillates; whether the specified version is
   stable is an empirical question.
3. **The virtual geometry pipeline (M7).** Cluster hierarchy construction, crack-free simplification
   and GPU traversal are each hard, and the visibility buffer couples them to material resolve.
4. **Determinism across the whole simulation (M2 hooks, M9 validation).** The hooks are cheap; the
   discipline is not. This is the risk most likely to be discovered late, which is exactly why the
   validator's hooks are pinned to M2.
5. **The live bridge and hosting modes (M5).** An out-of-process editor is the right decision and
   the expensive one; latency and state synchronisation are where it is paid.

Each is scheduled as a spike at the head of its milestone, under the "risk is scheduled, not
discovered" requirement, on the principle that a decision is a deliverable.

## Alternatives considered

**A time-boxed roadmap with quarters.** Rejected: see "why there are no dates". A quarter-based
plan for a project with no code and no team size is fiction that costs credibility later.

**Feature-area workstreams instead of milestones.** Rejected as the horizontal failure mode by
construction. Workstreams optimise for parallelism the project does not have and hide integration
cost it cannot afford.

**Roadmap as documentation only, no specification.** Rejected. Documentation drifts silently;
specifications are validated and changed deliberately. The rules in this capability — invariant
timing, dependency ordering, gate persistence — are exactly the kind of thing that gets quietly
abandoned when it is only prose. Keeping the ladder normative is also what lets the documentation
be a *view* rather than a second source of truth.

**Tracking status in an issue tracker rather than the repository.** Rejected for the same reason
the specifications are in the repository: the status record must be checkable against the
specification set by a recipe, in the same commit as the work.

## What this change does not decide

- **How long anything takes.** By construction.
- **Who does what.** The roadmap is ordering, not assignment.
- **The internal task breakdown of a milestone.** That belongs to the OpenSpec change that
  implements it; the roadmap states the entry conditions and the exit criteria only.
- **Whether a deferred capability is ever built.** It records the seams and the re-entry point.
