# Design: M11.e — Ship

## Navigation and physics implementation following the evidence sweep

The 6.1a map identifies seven requirements that currently answer through M11.e exemptions. Task
6.1b treats the baseline `openspec/specs/navigation/spec.md` and `openspec/specs/physics/spec.md`
as the behavior contract. Navigation worlds select a mesh and deterministic query queue by world
identity; planar and volume representations reuse the query concepts of area costs, masks,
budgets, partial paths and scheduled delivery. Debug data is emitted through engine-owned sinks,
with bounded counters and no renderer dependency. Physics constraints and optional simulation map
through `PhysicsServer` to Jolt, and capability flags reflect the operations actually supported.
The reference backend continues to report unsupported solver features explicitly. A mapping moves
from exemption to test only when the named case fails under a targeted behavior mutation.

### Optional simulation mapping for PR #8

The engine-owned physics interface gains descriptions and readback for cloth and vehicles, without
exposing Jolt types. A cloth description owns no caller memory after creation: vertices contain
rest positions and inverse masses (zero pins a vertex), while triangle indices define its surface
and spring topology. A soft body uses the ordinary generational `BodyHandle`, world ownership,
destruction, and collision filtering; a vertex-readback call lets a renderer consume the deformed
surface. Jolt constructs shared settings and edge/bend constraints, then stores the body in the
same slot table as rigid bodies. The reference backend reports `soft_bodies = false` and returns a
named `Unsupported` diagnostic. Neither backend may claim support before its creation, stepping,
readback, destruction, and conformance cases pass.

Vehicles are authored from a dynamic chassis body plus wheel placement and suspension/drivetrain
parameters. The Jolt vehicle constraint is registered as both a constraint and a step listener;
destroying the vehicle or chassis removes both registrations before freeing the chassis. Input
throttle, brake and steering is supplied before the fixed step, and wheel state is read after it.
The reference backend remains capability-gated. Ragdoll profiles are generated from a finalized
skeleton at the layer-4 physics/animation join: bone shapes and mass, parent constraints and limits
are editable asset data, not Jolt objects. Activation seeds body poses and velocities from the
current and previous animation poses; per-body weights vary continuously, with powered bodies
following targets through motors while still receiving contacts and impulses. Partial ragdolls
retain animation control outside the selected mask and blend at its boundary. The tests must prove
pose transfer, partial/full blending, powered impulse recovery and lifetime cleanup.
The powered path uses an engine-owned swing-twist orientation motor: the target is the child body
rotation relative to its parent in body space, with a torque cap and spring tuning. Updating the
target wakes sleeping dynamic bodies so animation changes reach the solver. This joint drive alone
does not satisfy ragdoll coverage; profile generation, activation and blending remain separate
requirements.
The layer-4 ragdoll module now supplies those pieces over `PhysicsServer`. Its generated profile
keeps an editable shape, mass, swing/twist limits and motor strength for each skeleton joint.
Profiles retain only value-owned sphere, capsule or box shapes; array-backed shape descriptions
would borrow transient caller buffers and are rejected before solver allocation.
Activation creates bodies at the current actor/model pose, derives linear and angular velocity from
the previous pose, and connects parent-child bodies with swing-twist constraints. Full mode drives
all bodies dynamically; powered mode retains an animation-driven root and motor-driven children;
partial mode uses a per-bone weight mask and kinematic zero-weight bones. A per-body timed blend
interpolates animation and solver pose continuously. Hits apply impulses and temporarily expose
the physical pose before recovering to the mode's base blend weight. The owner tears down joints,
bodies and shapes in that order; reference-backend activation refuses before allocating them.

The physics buoyancy requirement is not complete merely because `WaterSystem` computes lift and
drag: a layer-4 physics/water adapter must read a dynamic body's transform and velocities, rotate
its authored hull sample offsets into world space, query the authoritative water surface, and apply
the resulting force and torque through `PhysicsServer` before the fixed step. Both Jolt and the
reference backend can integrate those forces without a backend-specific water dependency. A solver
case must observe actual pitch from multi-point swell; the existing water-only case remains the
arithmetic and displacement-band contract, not the physics integration claim.

## 1. The spike, and why it has not run

**M8.c's design opened by saying there was no spike and explaining why; this one opens by saying
there is one and that it has not run.** That difference is the whole of this section, because the
worst thing this rung could do is scope five sections of mobile work against a hypothesis and
discover at its own gate that a hosted runner was never going to produce the artefact.

**The question, stated so it can only be answered by a measurement:**

> Can a hosted continuous-integration runner produce a **mobile artefact** — an `.apk`, an `.ipa`, or
> a cross-compiled shared library for one mobile ABI — from this repository's toolchain, and report
> it as a downloadable artefact, for one empty project and nothing else?

**Why it is a real question rather than a formality**, each clause verified on the tree this rung
starts from:

- `cmake/modules.cmake` line 39 declares the vocabulary — `set(CY_MODULE_PLATFORMS linux windows
  macos ios android visionos web)` — and line 317 states the truth in the build's own diagnostic:
  *"Supported: Linux, Windows, Darwin. Planned: iOS, Android, visionOS, Web."* **A module manifest
  may name `android` today and nothing downstream will build it.**
- `cmake/` holds seven files — `compilers`, `dependencies`, `features`, `module`, `modules`,
  `project`, `profiles` — and **no toolchain file of any kind**, for any target, cross or native.
  `build-system-and-platforms` requires *"cross-compilation through CMake toolchain files, with a
  documented toolchain per target"*; there are zero.
- **Seventeen dependencies are integrated and every one of them was fetched and built for a
  desktop.** SDL3 is the windowing layer on every target the engine has; a mobile port either
  replaces it, or discovers which of the seventeen has no mobile build, one dependency at a time,
  inside this rung.
- `ci.yml` has six build legs and six test legs across `linux`, `macos` and `windows`, each on
  x86-64 and ARM64, and **not one of them cross-compiles anything**.

**Four axes the spike has to separate**, because "mobile does not work" is not a finding:

| Axis | The question | What a negative answer costs |
|---|---|---|
| **Toolchain acquisition** | Can a hosted runner install an NDK or an Xcode iOS SDK inside a job's time budget? | The mobile scope becomes a self-hosted-runner problem, which this project has no runner for |
| **Configure** | Does `cmake` configure this tree for one mobile ABI with a toolchain file, with every enabled dependency resolving? | Names the dependencies that have no mobile build, which is the real list and not a guess |
| **Build and link** | Does one module — the smallest, `cy::core` — compile and link for that ABI? | Separates "the build system cannot cross-compile" from "a dependency cannot" |
| **Artefact** | Does the job publish something a person could install or load? | Separates a compile claim from a ship claim, which is the distinction this whole rung is about |

**The spike is a throwaway prototype under `~/cyberdyne-spikes/m11e-mobile-spike/`, outside the
repository**, as M3's, M5.5's, M6's, M7's, M8.b's, M9's and M10's were, because a prototype under
`docs/` fails `just quality-layers`, correctly. Its only deliverable is a decision.

**The contingency is written before the answer, so the answer cannot be negotiated afterwards:**

- **If all four axes clear** — mobile is in scope, sections 1 and 2 are real work, and
  `rendering-forward-clustered` reaches Complete on a pipeline that has run.
- **If toolchain acquisition or configure fails** — **mobile becomes a recorded deferral with a
  re-entry point**, this rung is the distribution and record rung, and `build-system-and-platforms`
  and `rendering-forward-clustered` are re-scoped through changes against their own specifications
  rather than claimed thin. That is a finding worth having on day one of the rung rather than at its
  gate, and `delivery-roadmap`'s *"a spike may resize a milestone as well as redirect it"* is the
  requirement that permits it.
- **If build and link clear but the artefact does not** — the honest claim is a **cross-compilation**
  claim, the artefact criterion reports **NOT EVALUATED** through `where = "ci"` with its reason, and
  the distribution half of the row is deferred rather than the whole row.

**What the spike may not do is decide in this document's favour.** M10's spike overturned the
prediction its own design.md had written — §4 there predicted `procedural-content-generation` would
be the demotion and the measurement said the row's scope survives intact — and that is the standard
this section is held to. A spike that confirms the plan is a spike that was not worth running.

## 2. What this rung depends on and does not own

**This is the last rung on the ladder, so nearly everything it is measured on was built by someone
else.** Stating that as a table is not bookkeeping: three of the four rows this rung carries cannot
close on work this rung performs.

| What | Owned by | Why this rung needs it |
|---|---|---|
| The CI job that publishes one leg's digest and compares it with another's | **M11.a** | Section 5's full matrix is that job generalised. Building it twice would be two jobs disagreeing about what a leg is |
| Criteria that *evaluate* `build-system-and-platforms`' and `thirdparty-dependencies`' **Working** tier | **M11.a** | Both are recorded at Seed with a column claiming Working and **no criterion in any of the fifteen ledgers evaluating either**. A Complete cell on top of an unchecked Working tier is the defect M10's gate refused twice |
| Steam Audio integrated — `m8c:steam-audio-configures` | **M11.a** | It is an **adoption against this rung's row**: `thirdparty-dependencies` requires the evaluation recorded through the change flow, and the manifest entry exists while the fetch has never run |
| mbedTLS, for `save-and-persistence`'s confidentiality half | **M11.a** | Same shape. A second adoption this rung's row governs and another rung performs |
| HarfBuzz, ICU, FreeType, msdfgen — `text-and-fonts` | **M11.b** | Four more. `text-and-fonts`' Complete cell moved to M11 explicitly *"where three third-party integrations belong"*, and none of the three is in `deps/manifest.toml` |
| Image codecs and texture encoders — the material and image rows | **M11.c** | `libpng`, `libjpeg-turbo`, `libwebp`, `tinyexr`, an ASTC and a BC encoder; a picture made of real materials needs textures that were decoded and encoded by something |
| MSAA and multi-view — `rendering-forward-clustered`'s **desktop** half | **M11.d** | This rung owns the row's Complete cell and the mobile third of its scope. **Two rungs, one row, and the tier is this rung's to record** |
| `build-and-packaging` — content audit, provenance, symbols, DLC, distributed execution | **M11.d** | Distribution artefacts are assembled by the packaging capability. If M11.d demotes that row — its own proposal predicts it may — its remaining scope arrives here with the reason that demoted it |
| `developer-workflow-and-just` | **M11.d** | Two of the three stale refusals section 4 finds are its recipes, not this rung's |
| The engine's `lower_material`, the `Domain::Materials` vocabulary and its pins, and the interchange `cy_material author` canonicalises | **M11.c** | **Inherited scope rather than a dependency this rung waits on — tasks section 5a.** `specialised/graph.rs` assigned *writing `.cygraph` from Rust* here before M11.c started; M11.c's spike then found junction 1 REFUSING and built two of the three pieces behind that refusal. The third — a front end a person drives, which is a front end that SAVES — was always this rung's, and it arrives with everything under it already built and tested |

**And one boundary inside a shared specification.** `delivery-roadmap` is this rung's row, but
M11.a's split change also writes against it — the ladder-insertion mechanics, `record.MILESTONES`,
`gates.toml` and `selftest.MINIMUM_CRITERIA` for five rungs. **This rung's delta against that
specification is limited to three things it alone is responsible for**: the deferral discipline of §3,
the 1.0 record as a criterion, and the refusal-label check of §5. The ladder mechanics are M11.a's
and are not restated here.

**The consequence worth naming, because it is this rung's defining structural risk:
`thirdparty-dependencies` at Complete is a row this rung cannot finish alone.** Six to ten library
adoptions are performed by M11.a, M11.b and M11.c, each against a requirement on this row, and this
rung records the tier. If any of those rungs defers an adoption, the deferral lands on this row and
this rung has to say so rather than count the library as integrated because a manifest entry exists.
**A manifest entry is not an integration** — `steam_audio` has had one since M8.c and does not
configure.

**One inherited task is finished by making another rung's green criterion go red, and it is the only
one on the ladder with that shape.** `m11c:the-shot-does-not-overclaim-the-editor` asks
`cyberdyne-editor --list-commands` for its registry and fails the moment a `material.*` command
appears in it, because M11.c's beauty shot **was authored through the tooling that exists rather than
through a material graph editor** — its materials placed and wired on the editor's authoring model by
a binary in the editor's workspace, not by a person at a window and not over the control socket — and
the artefact's caption says exactly that. Task 5a.1 makes the caption false by succeeding. **The
caption is then what changes, in the same change, and the criterion is not weakened to keep it
green**; a criterion written to fail when the tree improves is a criterion doing its job, and this is
the ladder's first instance of one.

## 3. The sweep, and the discipline that makes it honest

**The sweep is where a milestone lies to itself.** Every row that an earlier rung could not finish
arrives here, and the temptation at a 1.0 gate is a line in a document that reads *"deferred"* and
means *"we stopped"*. `delivery-roadmap` already forbids the weak version — deferred scope *"SHALL
NOT be silently dropped"* — but "not silent" is a low bar and this rung needs a high one.

**A deferral is only honest with three things, and this rung refuses one that has fewer:**

1. **What is unmet**, requirement by requirement, read against the tree at the gate — not the scope
   statement that was written before the work started.
2. **Why it is deferred** — the measurement or the decision, named. "Ran out of milestone" is a
   schedule, not a reason; `save-and-persistence` arrives here with a reason (a dependency adoption
   that is a change of its own) and that is the shape.
3. **The condition that brings it back** — a state of the world, not a date. `docs/roadmap/risks.md`
   already carries eleven deferrals in exactly this form, with the seams held open and the check that
   proves they are still open. That table is the template and this rung extends it rather than
   inventing a second list.

**A row arriving here with none of those three is a row that has to be finished, not recorded.**

**And the sweep's own claim has to be checkable, which is the lesson of `m9:record-matches-plan-history`.**
That gap is four rows whose Working tier a matrix column claims and **nothing evaluates**: the only
`expect_tiers` entry naming any of the four is `m0:roadmap-tiers` expecting `seed`, and `_check_tiers`
treats an exit tier as a **floor**, so `seed` can never contradict a Working claim. M10 task 6.5
closed that gap by *recording* the four, and M10's closing gate put the declaration back, because a
tier written into a file that nothing re-checks is not a tier that was checked.

**So the 1.0 record is not a document. It is a criterion**: every row of the record is Complete, or
carries a deferral with all three parts, and the check reads the record and the deferral list and
fails naming any row that is neither. That criterion can go red today, on 66 rows, which is what
makes it a criterion.

## 4. Which rows are contingent, and the demotion this design predicts

Four rows and a sweep is the smallest row count on the ladder and, on this design's own reading, the
largest spread of outcomes. The honest position at proposal time is that they are not equally risky.

| Row | Contingent on | Reading |
|---|---|---|
| `rendering-forward-clustered` (11 reqs) | **The spike, entirely** | Two thirds of its remaining scope is M11.d's. What is left here is the five mobile pipeline differences — no depth prepass, per-object light lists, tonemapping as a subpass, the omitted screen-space effects, memoryless attachments — and **every one of them is a property of a pipeline running on a tiled GPU**, which is hardware nobody on this project has |
| `build-system-and-platforms` (13 reqs) | **The spike, and M11.a** | Cross-compilation, distribution artefacts and the CI matrix. The first is the spike's subject; the second needs M11.d's packaging; the third generalises M11.a's job. Its Working tier is one of the four nothing evaluates |
| `thirdparty-dependencies` (9 reqs) | **M11.a, M11.b and M11.c** — §2 | The row this rung is least able to finish by working harder. Its Working tier is also one of the four nothing evaluates |
| `delivery-roadmap` (22 reqs) | **Every rung** | The only row whose Complete cell is *about* the other rows. It cannot close before the sweep does, and it closes last by construction |

**The demotion this design predicts is `rendering-forward-clustered`, and it predicts it now rather
than at the gate.** The reasoning, which the spike is what tests:

A tiled GPU is the subject of all five differences, this project has no tiled GPU and no mobile
runner, and `ci.yml`'s hosted legs are x86-64 and ARM64 desktops. The mechanisms can be *built* —
memoryless is a render-graph attachment declaration, a subpass is a pass-order property, per-object
light lists are a culling mode — and every one of them can be **asserted against the declarations**
on the null backend without a tile in sight. What cannot be done here is the claim the requirement
actually makes: *"WHEN the mobile pipeline runs on a tiled GPU THEN the HDR colour attachment SHALL
be declared memoryless and resolved to the swap chain within the same render pass."* That scenario
names the device.

So the predicted outcome is **Complete on the declarations with the device scenario reported NOT
EVALUATED**, which `delivery-roadmap`'s own machinery supports and which this rung's specification
delta makes explicit — or, if the gate judges a row cannot be Complete while the scenario that names
its hardware has never run, **the row is demoted to Working with the mobile half deferred and a
re-entry point of "a mobile device or a mobile runner exists"**. Either is defensible; a green tick
over an unphotographed tile is not.

**The prediction this design would most like to be wrong about is the opposite one**: that the spike
clears all four axes, an Android artefact comes out of a hosted runner in an afternoon, and the
mobile scope turns out to be the cheapest thing in M11 rather than the thing that was left last
because it was feared. M10's spike was wrong in that direction and the milestone was better for it.
**This design has no measurement either way, and says so rather than guessing convincingly.**

**And the rung's prediction about the ladder as a whole**: if any rung closes with rows still open, it
is this one, and the correct outcome is **1.0 recorded with a named deferral list** rather than 1.0
delayed until a phone works. `delivery-roadmap`'s exit says *"every capability is Complete or
explicitly deferred"*, and the second half of that sentence is the mechanism, not the loophole — but
it is only a mechanism if §3's three parts are enforced.

## 5. Three classes of refusal naming the wrong milestone — and why that is a check

This is small and it is the exact shape of defect the roadmap's own gates keep finding, so it is
written down as a mechanism rather than as four edits.

| Recipe | Refuses naming | The tree says |
|---|---|---|
| `release-version`, `release-changelog`, `release-artefacts`, `release-publish` | **"M12 — build-and-packaging"** | **There is no M12.** `record.MILESTONES` is fifteen entries ending at `m11`, and `build-and-packaging` reached Working at M6 |
| `build-shaders` | **"M3 — the renderer and its shader toolchain"** | M3 closed **eight rungs ago**. `slangc` is integrated — `cmake/dependencies.cmake` line 303 — and there are 26 `.slang` modules in the tree. `build-all`'s own description still reads *"Shaders join at M3"* |
| `maintenance-clean` | **task "2.1.5"** | A task number rather than a milestone, and `build-clean` sits directly below it doing the job |

**The same defect was found and corrected once already**, in `just/content.just` line 149:
*"`build-and-packaging` reached Working at M6, not M12: the roadmap moved it and these two recipes"*.
It was corrected by hand, in one file, and the other six were not — which is precisely what makes it
a check rather than an edit. **A recipe that refuses SHALL name a rung on the ladder that has not yet
closed**, and a check that reads `just/` and `record.MILESTONES` fails today, three times, naming
each one.

Two of the three belong to `developer-workflow-and-just`, which is **M11.d's row** — §2. This rung
owns the check and the four release recipes; if M11.d has not corrected the other two by the time the
check lands, correcting a stale label is a one-line edit and not an absorption of another rung's
capability. That distinction is stated here so a later reader does not have to reconstruct it.

## 6. What this rung deliberately does not do

- **No second desktop backend and no native platform layer.** Metal, D3D12 and the native
  `Platform`/`DisplayServer` are M11.d's, and starting a fourth backend here would validate the RHI
  against a guess rather than against M11.d's two working implementations —
  `delivery-roadmap`'s *"a second backend SHALL be started only after the first has passed a milestone
  gate"*, applied to the rung below.
- **No XR.** `xr-support` stays deferred with its re-entry recorded as *"after 1.0"*. What this rung
  owes it is that the prerequisite checks still pass on the day 1.0 is recorded, **and that the one
  prerequisite whose seam is half open is named as half open** — `tests/render/test_xr_prerequisites.cpp`
  and `m3.toml`'s own note both record it: `cy::Runtime::tick()` takes no predicted display time, so
  a host cannot tell the engine when a frame will be displayed. A deferral whose seam check reports
  green over a half-open seam is worse than no check.
- **No new deferrals invented for convenience.** This rung is the only one on the ladder permitted to
  record a deferral, and that permission is the reason §3's three parts are mandatory rather than
  advisory.
- **No web and no visionOS.** `CY_MODULE_PLATFORMS` names both and the M11 scope names neither. They
  stay in the planned list, unclaimed, with `core-platform-abstraction`'s obligation not to preclude
  them unchanged.
- **No tier recorded here.** Recording a tier is the closing gate's act. Section 9 writes the ledger
  and section 10 is the gate; nothing in between may edit `docs/roadmap/status.yaml`.
