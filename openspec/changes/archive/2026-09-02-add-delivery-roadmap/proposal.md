# Add the delivery roadmap: a dependency-ordered ladder from empty repository to 1.0

## Why

There are 73 capability specifications, 1,155 requirements, 2,551 scenarios, and no code. Every architectural
question has an answer; the question that has no answer is **what to build first**, and that
question is not smaller than the others.

It is not smaller because ordering is not a scheduling detail here — it is a design consequence.
The specifications commit to a set of invariants that are cheap to establish and ruinous to
retrofit: stable field identity that every serialized artefact encodes, computed barriers that no
pass writes by hand, a commit boundary that every line of simulation code assumes, transactions as
the only write path, one command stream as the only input to the simulation. Each of those is a
property of the code written *after* it, not of a subsystem. Establish them late and everything
built in the meantime has to be revisited; establish them at the right point and they cost almost
nothing.

So the roadmap is a specification, not a plan. It states which capability must exist before which
other capability can be written honestly, what "exists" means at each stage, and what closes each
stage. It carries no dates: a date is an estimate that decays, whereas the fact that the render
graph must own barriers from the first pass is true regardless of how long the first pass takes.

The second reason is failure mode. A greenfield engine with this much specified scope fails in one
of two ways — a horizontal build where twelve subsystems are each 80% finished and nothing runs, or
a drift where the implementation quietly settles a question the specifications had already decided.
Vertical slices with executable exit criteria address the first. A status record that a recipe
checks against the specification set addresses the second.

## What Changes

- **New `delivery-roadmap` capability** — the ordering contract: milestones, tiers, gates, and the
  rules that keep them honest.
- **A twelve-milestone ladder, M0 to M11**, ordered by dependency and risk and carrying no dates.
  Each milestone ends in a runnable artefact committed to the repository and exercised by
  continuous integration — a Swift character controller at M4, an editor that survives a runtime
  crash at M5, a streamed multi-kilometre world at M6, a four-player rollback session at M9.
- **Four maturity tiers** — `—`, Seed, Working, Complete — because most of these capabilities span
  five milestones, and "implemented" is not a boolean for any of them. A capability reaches Seed at
  the milestone its first dependent needs it, not the milestone at which it is interesting.
- **A table of retrofit-hostile invariants**, each pinned to the milestone at which it must land
  even though its capability is nowhere near Complete. This is the part of the roadmap that is
  load-bearing.
- **Executable exit criteria**: every milestone closes on a recipe that exits zero, and once green
  its checks join the permanent gate set. A later milestone may not break an earlier one's gate
  without landing the replacement in the same change.
- **A backend and platform ladder** — null and Vulkan at M3, Metal seeded at M7 and delivered at
  M11, D3D12 last; three desktop platforms from M0, console porting surface and mobile at M11, XR
  deferred with its prerequisites checked.
- **Staged third-party integration** — each library enters at the milestone that first needs it,
  never before its engine-owned interface exists, with the stub retained so replaceability is
  demonstrated rather than asserted.
- **Deferred scope with re-entry points** — XR, fluids, hair, voxels, offline simulation import,
  authority migration, distributed builds, cloud saves, audio middleware: each records the seams
  that must stay open and the check that proves they are.
- **Risk scheduled first** — each milestone names its most uncertain work, and that work is spiked
  before the milestone commits. A spike may violate the ladder; its prototype may not be merged.
- **A single status record** — capability, tier, milestone, and the change that advanced it —
  updated by the change that does the work and checked by a recipe against the specification set.
- **Documentation** — `docs/ROADMAP.md` as the narrative view, with the capability matrix,
  dependency graphs, and risk register beneath it.

## Capabilities

### New Capabilities

- `delivery-roadmap` — milestone ladder, maturity tiers, executable gates, invariant timing,
  dependency ordering, backend and platform ladders, staged integration, deferred re-entry points,
  scheduled risk, the status record, and forbidden roadmap patterns.

### Modified Capabilities

- `developer-workflow-and-just` — add a **Roadmap** recipe category to the recipe surface: a status
  recipe that reports tiers and fails on drift, and a milestone recipe that runs exit criteria.
- `testing-and-quality` — merge gates include the exit criteria of every milestone already closed,
  so a closed milestone stays closed.

## Impact

- **No code.** This is a specification-stage change; it constrains how the implementation is
  sequenced, not what it does.
- **Every future change** acquires two obligations: state the capability and tier it advances, and
  keep the status record in step.
- **Documentation**: adds `docs/` with the roadmap, the capability matrix, the dependency graphs,
  and the risk register; the root README gains a roadmap section.
- **Risk**: the ordering itself. The most likely error is not that a milestone is too large but
  that a dependency was missed — which is why the specifications, not the roadmap, are
  authoritative about dependencies, and why re-sequencing is a change that must state what was
  learned.
