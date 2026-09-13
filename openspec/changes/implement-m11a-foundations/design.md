# Design: M11.a — Foundations

## 1. The spike, and what it has to answer before anything else is scoped

**This rung's named risk is that the 122 ms frame is not a shader problem.** Every estimate below
assumes it is. `m10:world-frame-budget` names three bands — the substrate re-sampled at every terrain
vertex (63.0 ms), the cloud march (23.2 ms) and water's foam field (12.0 ms) — and says the gap
"closes when those shaders exist, not by optimising the CPU loop". That sentence is a hypothesis
written by the milestone that measured the bands, not a measurement of the fix.

So the spike ports **one** band and times it: the 63.0 ms substrate re-sample, on the device, through
the sampler `cy/field.slang` will become, against the CPU path at the same positions and the same
resolution. It runs at the head of the rung, outside the repository, and its only deliverable is a
decision — `docs/roadmap/risks.md`'s rule, and the shape M3, M5.5, M6, M7, M8.b, M9 and M10 all used.

**Two numbers, not one.** A GPU sampler that is fast and disagrees with the CPU sampler is not a
result: `environment-fields`' *CPU and GPU access* requires "the same value for the same position and
resolution", and a band that got cheap by answering differently has moved the defect rather than the
cost.

### 1.1 What the spike must refuse to report

M10's spike exits 2 when its own harness is non-deterministic and exits 3 when its contention meter
measured nothing, and `README.md` in that spike lists the four mutations run against its finished
report. The same discipline, and the failure modes here are specific:

- **the device path never executed** — a fallback to the CPU sampler that reports a speed-up of one;
- **the comparison sampled outside the written tiles** — twenty-five samples of the declared default
  agree perfectly and prove nothing, which is `m10:sky-field-round-trip` exactly, one level up;
- **both paths returned the declared default** — agreement produced by absence, the same shape again.

Each of these SHALL exit non-zero saying which it was, rather than printing a number.

### 1.2 What the spike cannot answer here

This host has one GPU vendor. "The GPU sampler agrees with the CPU sampler" measured here is an
agreement with one driver's floating-point behaviour, which is not the claim — and it is the same
refusal `m10:pcg-gpu-domain-agreement` already carries as `where = "ci"` with a reason. The spike
reports the cost honestly and the *cross-vendor* half is section 4's job, not section 0's.

### 1.3 What each outcome costs the plan

| The spike finds | What this rung becomes |
|---|---|
| The band recovers on the device and the answers agree | §2 is ordinary engineering, the other two bands follow the same shape, and this rung's twelve rows are as scoped |
| The band recovers and the answers **disagree** | The sampler is the work and the *agreement* is the criterion. `environment-fields` does not reach Complete on a sampler that is fast and wrong, and the rung's schedule absorbs a correctness problem it has not budgeted |
| The band does **not** recover | **The 122 ms figure is not a shader problem and every estimate in this rung is wrong.** The five world rows stop being "large but well understood", §5's contingency fires, and the rung is re-scoped around whatever the measurement says the cost actually is — before section 2 starts, not at the gate |

The answer, and the numbers behind it, are written back into this section the way M10's design §1
consumed its spike, so that no row downstream re-derives them.

## 2. Why this rung exists at all, and what decides its boundary

M11 as written is sixty-five Complete cells in one milestone. The split's rule is in this change's
`delivery-roadmap` delta: **a milestone whose rungs are judged on different artefacts has different
gates**, and M8's split is the mechanical precedent — one change directory per rung, one ledger per
rung, `record.MILESTONES` extended in position, one gate and one `MINIMUM_CRITERIA` floor each.

M11.a's boundary is not a count and not a subsystem. It is: **a row belongs here when the thing
between it and Complete is a criterion that is already red.** Twelve rows satisfy that, carrying 233
requirements, and all twelve are at Working today. Everything else in M11 is blocked by work nobody
has started, which is the other four rungs.

That boundary has a consequence worth stating plainly, because it is the whole argument for running
this rung first: **the engine's remaining debt is concentrated in the substrate.** Three of the seven
inherited gaps — `fields-sampled-on-a-device`, `world-frame-budget` and, through its consumers,
`sky-field-round-trip` — are the same absent thing seen from three directions. One shader module
closes all three or none.

## 3. The seven gaps are the plan, and the ledger is what makes that true

Nothing in sections 2 to 7 of `tasks.md` is new scope. Each item is a criterion that runs on every
evaluation, fails today, and names its closing rung — and the mechanism is symmetric in a way that
matters more than the list: **a declared gap that starts passing fails the ledger** until its
declaration is deleted. A gap cannot be quietly fixed and cannot be quietly forgotten.

Two mechanical points follow, and both are forcing functions rather than good intentions:

- **Re-pointing is compulsory, not optional.** `criteria.py::_check_known_gap` rejects a
  `known_gap_closes` that is not in `record.MILESTONES`. The instant `m11` leaves the tuple, all
  seven ledgers refuse to load. There is no state of the tree in which the split has landed and a gap
  has been left pointing at a rung that does not exist.
- **NOT EVALUATED is never a pass.** `m10:pcg-regeneration-cross-platform` and
  `m10:pcg-gpu-domain-agreement` report NOT EVALUATED today through `where = "ci"`, and each `run`
  looks for the only shape that could answer it — a job that publishes one leg's digest and compares
  it against another's. A green there means the comparison exists, not that a single-leg suite ran.
  That is M9's `lockstep-cross-platform` lesson kept deliberately.

`m9:record-matches-plan-history` is the gap whose closure is a **method** rather than a fix, and it is
worth naming why it re-opened. M10 task 6.5 closed it by recording four rows at the tier their
columns claimed; M10's closing gate put the record back, because no criterion anywhere evaluates any
of the four — the only `expect_tiers` entry naming one of them expects `seed`, and `_check_tiers`
treats an exit tier as a **floor**, so `seed` can never contradict a Working claim. A tier recorded
because a document argued for it, with nothing that re-checks it, is the same defect as a column that
outlives its evidence. So §5 of `tasks.md` writes criteria that *evaluate* the four rows, and leaves
the recording to the gate that can then be contradicted.

## 4. One vegetation-potential: a modelling decision the specifications have already taken

`m10:fields-one-vegetation-potential` reads as a clash of two declarations, and it is really a
question the specifications answer if they are read together:

- `weather-and-wind` *Ecosystem state*: "Weather and environment SHALL maintain **macro ecosystem
  state** as fields — vegetation density, biomass, forest age, soil health, burn fraction, moisture …
  evolving toward **biome potential**".
- `environment-fields` *Potential and current state*: potential and current state SHALL be
  **distinct**. "**Potential** — biome potential, vegetation potential — derives from slow inputs …
  **Current state** — current biome, vegetation density, burn fraction — reflects what events have
  left."

So `vegetation-potential` is the ecosystem's field and belongs to `cy::weather`; foliage's realised
`vegetation` is the current state and belongs to `cy::foliage`; and the defect is that
`src/foliage/src/system.cpp` declares the **potential** as well, Scalar/UNorm8/Static/`Persistent`,
against weather's UNorm16/SlowlyVarying/Authoritative. Two encodings of one quantity is exactly the
fork §2 of M10's design was shaped to make impossible, one namespace down.

The decision this change records: **foliage consumes the potential and declares only the current
state.** `specs/foliage/` and `specs/environment-fields/` carry it. `integration.standard_fields`
asserts the known state and therefore goes red the day it lands — deliberately, and the commit that
lands it says so rather than letting a red look like a regression.

## 5. Which rows are contingent, and what this rung predicts about itself

M10's design §4 stated its contingencies in advance and named the row it expected to demote. The
spike then measured that row and the prediction was **wrong in the useful direction** — `procedural-
content-generation` kept its scope and gained four requirements. That is the standard, and this is
this rung's version of it.

**`environment-fields` is the rung**, the way `environment-fields` was the milestone at M10. The
device-side sampler is the one piece of work three gaps point at and the only credible route to the
budget. If it lands, `terrain`, `water`, `foliage` and `weather-and-wind` are large but well
understood. If it does not, none of the four moves — and neither does M11.c, whose atmosphere row is
judged on the same cloud march.

**The demotion this design predicts is `save-and-persistence`, and it says so before the gate does.**
Nine of the eleven pieces of work are ordinary engineering; requirement 15 is a vetted AEAD, and
adopting a dependency "SHALL go through the OpenSpec change flow recording the evaluation against
these criteria" with a key-management story attached. That is why §6.1 of `tasks.md` carves
confidentiality out into its own change with a re-entry point rather than carrying it here: a row
that would be demoted for one requirement out of twenty is a row that was scoped wrong.

**And a third demotion would not mean it is late either.** It would mean the row should be split
through a change against its own specification, and that decision belongs in this rung rather than in
a fourth gate. This design commits to that in advance so the gate is not the place it gets decided.

**`audio` is the row most likely to be deferred rather than completed**, and the cost is measured
rather than guessed: four upstream dependencies and a `-fabi-version=6` patch that breaks GCC, in
`deps/manifest.toml`, with the fetch and build of upstream never run. If the ABI flag holds, the
honest outcome is a recorded deferral with its re-entry point. A Complete cell over a backend that
returns `NotImplemented` is not available.

**`m9:lockstep-cross-platform` is not a code question, it is a runner question.** Every leg the
comparison job compares is a leg nobody here can reproduce. The job either publishes digests that
agree or digests that disagree, and a disagreement is a finding this rung reports rather than a
failure it hides. The criterion exists to be capable of going red.

**`networking-and-replication`, `replay-and-rollback` and `simulation-and-determinism` carry 67
requirements between them and no named blocker**, which is a different risk from the others: nothing
has refused them because nothing has read them end to end at Complete grade. The mitigation is
scheduling, not engineering — read all three against the tree **before** section 7.5 is estimated,
the way `save-and-persistence` was read at M10's close, and expect that read to find work the plan
does not currently carry. `save-and-persistence` went from two blockers to eleven pieces under
exactly that treatment.

**What this design does not predict**: that the spike will fire §1.3's third row. It is named because
it is the outcome that invalidates the rung, not because it is expected — and if it fires, the
correction belongs at the head of the rung where it is cheap, not at the gate where it is a demotion.

## 6. What this rung depends on other rungs for, and does not absorb

| Boundary | Who owns it | Why the line is there |
|---|---|---|
| `atmosphere-sky-and-clouds` → Complete | **M11.c** | `m10:sky-field-round-trip` is declared against that row and closes **here**, because it is a defect in a field's write path and this rung owns the substrate. The row's **tier** is M11.c's, because the rest of what it needs is the picture: the cloud march tuned, and the GI seam `dependencies.md` cycle 2 is about. A gap closing in one rung and a tier being recorded in another is the normal shape when a row's last requirement is not its only one |
| The editor rows, `editor-architecture` and `live-editing` off Seed | **M11.b** | This rung touches no editor code. If M11.b finds that the two rows are a mis-record rather than a thin foundation, that is M11.b's finding |
| Metal, D3D12, the native `Platform`/`DisplayServer` | **M11.d** | `cy/field.slang` is written against the RHI that exists. A second backend would double the device half of §2.2, and that is deliberately after this rung, not inside it |
| The **full** CI matrix, mobile legs, distribution | **M11.e** | §4 builds **one** upload-then-compare job over legs `ci.yml` already has. Turning three legs into the whole matrix is M11.e's |
| `build-system-and-platforms`, `developer-workflow-and-just`, `testing-and-quality`, `thirdparty-dependencies` → **Complete** | **M11.d and M11.e** | §5 writes criteria that evaluate their **Working** tier, which is the half `m9:record-matches-plan-history` is about. Completing them is different work in a different rung, and this rung does not claim it |
| The confidentiality half of `save-and-persistence` | **its own change** | A dependency adoption under `thirdparty-dependencies`' rule, with a key-management story. §6.1 carves it out with a re-entry point rather than leaving it to be the thing that demotes the row a third time |

## 7. What this rung deliberately does not do

- **No new renderer architecture, and no second field mechanism.** `cy/field.slang` is a sampler over
  the layout `gpu.h` already fixes and tests on the CPU. If the layout is wrong the spike says so;
  a second layout for the device is the fork this whole substrate exists to prevent.
- **No tier cells.** Recording a tier is a closing gate's act. This change writes plans, criteria and
  specification deltas; `status.yaml`'s tier column is not its to edit.
- **No CPU-side rescue of the frame budget.** A CPU optimisation that reaches 16.7 ms closes nothing,
  because `m10:world-frame-budget`'s own text says what closes it. The temptation is real —
  63.0 ms of re-sampling per terrain vertex has obvious CPU-side wins in it — and taking them would
  produce a green artefact over an open gap, which is the one unforgivable outcome.
- **No renumbering of the ladder.** Five rungs are an insertion, as M5.5's and M8's were, and every
  existing reference to M11 stays valid as the name of the group.
