# M11.a — Foundations: the debts paid, and the frame budget made real

## Why

**M11 as written is sixty-five Complete cells in one milestone, and seven of the criteria that would
have to go green for any of them are failing today.** This is the rung that closes those seven, and
it exists because **nothing downstream of them is credible while they are open**: an editor rung that
authors content into a world costing 122 ms a frame is authoring into a world nobody can ship, and an
image rung tuning a picture that no shader samples the environment through is tuning the wrong thing.

**What is verified absent on the tree M10 closes on**, read out of the record rather than assumed:

- **Seven declared gaps, every one of them a running, failing criterion that names `m11` as its
  closing rung.** `tools/roadmap/milestones/` holds exactly seven `known_gap_closes = "m11"` entries
  and no others: `m10:sky-field-round-trip`, `m10:world-frame-budget`,
  `m10:fields-one-vegetation-potential`, `m10:fields-sampled-on-a-device`,
  `m8c:steam-audio-configures`, `m9:record-matches-plan-history` and `m9:lockstep-cross-platform`.
  Each has a `ci_job`, each runs on every evaluation, and a declared gap that starts passing **fails
  the ledger** until its declaration is deleted — so none of them can be quietly fixed and none can
  be quietly forgotten.
- **The world demo costs about 122 ms a frame with a device and about 106 ms headless against a
  16.7 ms budget — seven times over**, nearly flat across the day/night cycle. The three largest
  bands are the substrate re-sampled at every terrain vertex (**63.0 ms**), the cloud march
  (**23.2 ms**) and water's foam field (**12.0 ms**), and all three are work a shipping engine does
  in a shader.
- **`cy/field.slang` is unwritten and zero `.slang` modules sample an environment field.** There are
  26 `.slang` modules in the tree and 15 of them under `src/rendering/shaders/cy/`; the list is
  `brdf`, `cluster`, `color`, `frame`, `fullscreen`, `globals`, `light`, `material`, `noise`,
  `packing`, `particle`, `sampling`, `shadow`, `tonemap`, `view`. `src/environment/`'s README says
  `cy/field.slang` is owed by the first renderer-facing row to sample a field in a shader, and no
  renderer-facing row has.
- **A project that registers both `cy::foliage`'s and `cy::weather`'s producers fails at startup.**
  `vegetation-potential` is declared UNorm8/Static/Presentation-side by one and
  UNorm16/SlowlyVarying/Authoritative by the other, and `FieldRegistry::declare()` refuses the second
  in either order.
- **`CloudShadowField` writes tiles it cannot read back.** `update` reports writing tiles darker than
  0.5; `sample` returns the declared 1.0 at all twenty-five points inside `radius_metres`. Terrain
  and water round-trip through the same store. Nothing outside `src/rendering/sky/` reads the field
  at all, against a requirement that it be consumed by terrain, foliage, water and illumination.
- **No continuous-integration job compares one leg's result with another's.** `ci.yml` has
  `linux-arm64`, `macos-arm64` and `windows-arm64` legs in two matrices and they run independently.
  `m9:lockstep-cross-platform` fails for want of that job; `m10:pcg-regeneration-cross-platform` and
  `m10:pcg-gpu-domain-agreement` are reported **NOT EVALUATED** for want of the same one. **One job
  answers three criteria.**
- **`-D CY_AUDIO_STEAM_AUDIO=ON` does not configure**, and `SteamAudioBackend::simulate` returns
  `NotImplemented`. M8.c measured the whole cost — four upstream dependencies and an ABI flag that
  blocks both pinned compilers — and `deps/manifest.toml` records the measurement rather than a
  guess: the fetch and the build of upstream have never been run.
- **Four rows' Working tier is claimed by a column and checked by nothing.** `testing-and-quality`
  (M3), `build-system-and-platforms` (M4), `developer-workflow-and-just` (M6) and
  `thirdparty-dependencies` (M8.b) are recorded at **Seed**; the only `expect_tiers` entry naming any
  of them is `m0:roadmap-tiers` expecting `seed`, and `_check_tiers` treats an exit tier as a
  **floor**, so a Working claim can never be contradicted. `build-system-and-platforms` appears in no
  milestone ledger but `m0.toml`.
- **`save-and-persistence` is mis-scoped rather than late.** M9 demoted it, M10 demoted it a second
  time, and M10's `design.md` §4 wrote down in advance what a second demotion of one row means. Read
  requirement by requirement, its twenty requirements come out **nine satisfied, three unmet, eight
  partial**. On this tree, `benchmarks/` holds `ecs`, `gameplay` and `micro` and **no save
  benchmark**, though the requirement says the engine SHALL maintain one; and **the only thing
  outside `src/save/` that links `cy::save` is `samples/06-open-world`** — the ordered load pipeline
  and the translation between `world::PersistenceOverlay` and `save::Overlay` live in a sample rather
  than in the engine.
- **`samples/10-world` streams nothing.** It keeps the whole world resident, meshes every terrain
  tile at level 0 and reports `MeshReport::stitched_vertices` as zero precisely so a reader can see
  it, which is why `world-partition-and-streaming`'s Complete cell was moved off M10 by its own gate.

## What Changes

### The split, and the seam it follows — M11 `tasks.md` 0.1, answered

The capability matrix has said since M6 that M11 *"is the one milestone that could reasonably be
split, and the roadmap will split it through a change if the work turns out to be separable along a
real seam rather than an arbitrary one"*. Its load went 48 → 61 at M10's record audit and 61 → 65 at
M10's closing gate. **M11 becomes five rungs, and the seam is what each rung's artefact can be judged
on**, not a count:

| Rung | For | Artefact |
|---|---|---|
| **M11.a · Foundations** | the seven inherited gaps, the frame budget, the cross-leg job, save re-scoped | the world demo inside its budget, on a device, with the seven gaps closed or re-declared |
| **M11.b · Authoring** | the editor rows off Seed and the authoring surface finished | **a real sample game**, made through the editor |
| **M11.c · Image** | materials, shaders and everything the picture is made of | **an art-directed beauty shot**, authored through the editor M11.b finished |
| **M11.d · Desktop** | Metal, D3D12, a native platform backend, the desktop package | `samples/11-ship` on desktop |
| **M11.e · Ship** | mobile, the full matrix, distribution, the 1.0 record | every remaining row Complete or deferred with a re-entry point |

The rule this split follows, stated so the next reader does not re-derive it: **a milestone whose
rungs have different artefacts has different gates, and a gate that cannot name what it is looking at
is not a gate.** M8's split established the ladder-insertion mechanics — `record.MILESTONES` is an
ordered tuple, `gates.toml` gains one gate per rung, `selftest.MINIMUM_CRITERIA` gains one entry per
rung, and the matrix columns and load table move with them — and this change repeats them five times
rather than inventing a second mechanism. Every existing reference to M11 stays valid as the name of
the group.

### The work of this rung

- **A shader-side environment-field sampler.** `cy/field.slang` written against the layout `gpu.h`
  already fixes, bound through the GPU scene, and measured against a device. Closes
  `m10:fields-sampled-on-a-device`, whose current measurement is zero.
- **The three budget bands, in the order the gap names them** — the substrate re-sample, the cloud
  march, the foam field. The gap says in as many words that it closes when those shaders exist, not
  by optimising the CPU loop.
- **The sky's write path**, the two assertions parked in
  `src/rendering/sky/tests/test_cloud_shadows.cpp` restored as the check, **and the consumers** the
  requirement names.
- **One `vegetation-potential`**, which is a modelling decision across `foliage` and
  `weather-and-wind` before it is a code change. `integration.standard_fields` goes red on purpose
  the day it is answered, because it asserts the known state.
- **One CI job that publishes one leg's digest and compares it with another's**, answering
  `m9:lockstep-cross-platform`, `m10:pcg-regeneration-cross-platform` and
  `m10:pcg-gpu-domain-agreement`.
- **Streaming in the world artefact**, so `world-partition-and-streaming` is judged on a world that
  streams rather than on one that fits.
- **Steam Audio configured and simulating**, which is a dependency adoption before it is a backend.
- **The four unchecked rows evaluated.** Criteria that *evaluate* `testing-and-quality`,
  `build-system-and-platforms`, `developer-workflow-and-just` and `thirdparty-dependencies` rather
  than a tier written into a record nothing re-checks. Their Complete cells stay at M11.d and M11.e;
  what this rung owes is that their **Working** tier stops being an unexamined claim.
- **`save-and-persistence` re-scoped through a change against its own specification**, with the AEAD
  half taken as the dependency adoption `thirdparty-dependencies` requires it to be.

## Capabilities

**Twelve rows to Complete, all twelve from Working**, carrying **233 requirements** between them —
`environment-fields`, `terrain`, `water`, `foliage`, `weather-and-wind`,
`procedural-content-generation` (M10), `world-partition-and-streaming`, `save-and-persistence` (M6),
`simulation-and-determinism`, `replay-and-rollback`, `networking-and-replication` (M9) and `audio`
(M8.b).

The engine is **58 Working, 10 Complete, 7 Seed and 1 `none` of 76** today; these twelve are the
subset whose Complete cell is blocked by a criterion that is already red rather than by work nobody
has started.

## What is contingent, and what this rung predicts about itself

- **`environment-fields` is the rung.** The device-side sampler is the one piece of work three gaps
  point at, and the only credible route to the budget. If it lands, `terrain`, `water`, `foliage` and
  `weather-and-wind` are large but well understood. If it does not, none of the four moves and
  neither does M11.c, because the atmosphere row is judged on the same march.
- **`save-and-persistence` is the row this rung predicts it will demote**, and it says so before the
  gate does. Nine of eleven pieces of work are ordinary engineering; the confidentiality half is a
  vetted AEAD, and adopting a dependency *"SHALL go through the OpenSpec change flow recording the
  evaluation against these criteria"* with a key-management story attached. **A third demotion of
  this row would not mean it is late either** — it would mean the row should be split through a
  change against its own specification, and that decision belongs in this rung rather than in a
  fourth gate.
- **`m9:lockstep-cross-platform` is not a code question, it is a runner question.** This host has one
  operating system and one GPU vendor; every leg the comparison job compares is a leg nobody here can
  reproduce. The job either publishes digests that agree or it publishes digests that disagree, and
  **a disagreement is a finding this rung reports rather than a failure it hides** — the criterion
  exists to be capable of going red.
- **`audio` is the row most likely to be deferred rather than completed.** The cost is measured, not
  guessed: four upstream dependencies and an ABI flag that blocks both pinned compilers. If the ABI
  flag holds, the honest outcome is a recorded deferral with its re-entry point, not a Complete cell
  over a backend that returns `NotImplemented`.
- **`networking-and-replication` and `replay-and-rollback` carry 47 requirements between them and no
  named blocker**, which is a different kind of risk: nothing has refused them yet because nothing has
  read them end to end at Complete grade.

## Impact

- **New code**: `cy/field.slang` and the shaders for the three bands; the sky field's write path and
  its four consumers; the streaming binder `src/terrain/`'s README records as its largest gap; the
  save inspector, the semantic diff, the forbidden-pattern checks and a save benchmark; the Steam
  Audio simulation path.
- **Existing code**: one of the two `vegetation-potential` declarations goes away; `cy::save` gains
  the engine-side consumer that lives in `samples/06-open-world/` today.
- **Machinery**: one new CI job; five new ledgers under `tools/roadmap/milestones/`; `record.py`,
  `gates.toml`, `selftest.py`, the matrix columns and the load table all move with the split.
- **The seven gaps stay declared.** Re-pointing each `known_gap_closes` from `m11` to the rung that
  actually closes it is the correct edit; deleting one is correct **only** when the defect is fixed
  and the criterion is green.
- **Closing artefact**: the M10 world demo inside a 16.7 ms budget on a device, with the substrate
  sampled in a shader, the world streaming, and the ledger reporting each of the seven gaps either
  closed or still red with its reason.
- **Risk**, and the rung's named spike: **port one band — the 63.0 ms substrate re-sample — to a
  shader and measure it before scoping the other two.** If a GPU field sampler does not recover that
  band, the 122 ms figure is not a shader problem and every estimate in this rung is wrong.
