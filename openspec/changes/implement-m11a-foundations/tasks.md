# Tasks: M11.a — Foundations

Ordered. Section 0 is the spike and it runs first because **the whole of this rung's estimate rests
on one unmeasured assumption** — that the 122 ms frame is a shader problem. Section 1 is the split,
because the seven inherited gaps cannot be re-pointed at a rung the ladder does not contain, and
every section below it is a gap that is failing today.

Nothing in sections 2 to 7 is new scope. Each is a criterion that already runs, already fails, and
already names M11 as the rung that closes it, or a row whose Complete cell is blocked by one.

## 0. The spike — one band on a device, before the other two are scoped

- [ ] 0.1 **Measure, do not assume: port the 63.0 ms band and time it.** The substrate re-sampled at
      every terrain vertex is the largest of the three bands `m10:world-frame-budget` names, and it
      is the one whose shape a GPU field sampler most directly changes. Sample the same field, at the
      same positions, at the same resolution, on the device, against the CPU path, and report **the
      band's cost and the agreement of the two answers**. A sampler that is fast and disagrees is not
      a result
- [ ] 0.2 **What the spike must refuse to report.** M10's spike caught its own meter measuring
      nothing and exited non-zero rather than printing a matrix; do the same here. A run where the
      device path never executed, where the comparison sampled outside the written tiles, or where
      the two paths agree because both returned the declared default, SHALL exit non-zero saying so
      rather than reporting a speed-up
- [ ] 0.3 State plainly what the spike cannot answer on this host, through the ledger's own
      `requires`/`where` mechanism rather than as a sentence. One GPU vendor is one driver's
      floating-point behaviour, and `m10:pcg-gpu-domain-agreement` is already the precedent for
      refusing that claim here
- [ ] 0.4 Commit the spike outside the repository — `~/cyberdyne-spikes/`, as M3's, M5.5's, M6's,
      M7's, M8.b's, M9's and M10's were — and record its answer in `design.md` §1, where the rows
      that depend on it can read it without re-deriving it. **If the band does not recover on the
      device, design.md §5's contingency fires and this rung is re-scoped before section 2 starts**

## 1. The split — M11 `tasks.md` 0.1, answered, and the ladder it inserts into

- [x] 1.1 **M11 becomes five rungs, and the seam is the artefact.** M11.a Foundations, M11.b
      Authoring, M11.c Image, M11.d Desktop, M11.e Ship, each with its own change directory,
      proposal and README under `openspec/changes/implement-m11{a,b,c,d,e}-*/`, exactly as
      `split-m8-authorable-and-systems` gave M8.a, M8.b and M8.c theirs. The rule is written into
      `delivery-roadmap` by this change's `specs/` delta: a milestone whose rungs are judged on
      different artefacts has different gates, and a gate that cannot name what it is looking at is
      not a gate. **Done for this rung's `tasks.md`, `design.md` and `specs/`; the other four rungs
      own their own**
- [ ] 1.2 **`record.MILESTONES` gains `m11a` … `m11e` in position and loses `m11`.** M8's split is
      the precedent and the tuple is explicitly ordered for exactly this reason — M5.5's insertion
      sorted `m5b` to the end of the ladder and `criteria.rung` answered with the length of the
      list. `delivery-roadmap` already requires a test that asserts an inserted milestone's
      inheritance in both directions; this insertion is five and the test SHALL cover the order
- [ ] 1.3 **The seven declared gaps are re-pointed in the same commit, and this is forced rather than
      remembered.** `criteria.py::_check_known_gap` rejects a `known_gap_closes` that is not a
      milestone, so the moment `m11` leaves `MILESTONES` every one of the seven ledgers fails to
      load until its gap names a rung. Re-point each to the rung that actually closes it —
      `m10:sky-field-round-trip`, `m10:world-frame-budget`, `m10:fields-one-vegetation-potential`,
      `m10:fields-sampled-on-a-device`, `m9:lockstep-cross-platform`,
      `m9:record-matches-plan-history` and `m8c:steam-audio-configures` are all `m11a` by this
      rung's scope. **Deleting one is correct only when the defect is fixed and its criterion is
      green**
- [ ] 1.4 `gates.toml` gains five gates and `selftest.MINIMUM_CRITERIA` five floors, one per rung,
      each at `state = "joins-on-close"` until its own rung's gate has run. A green gate with no
      matching `milestone` job in `ci.yml` is the pattern `gates.toml`'s own header forbids
- [ ] 1.5 The matrix columns and the load table move with the split: the `M11` column becomes five,
      and every existing reference to M11 stays valid as the name of the group. **The tier cells
      themselves are each rung's closing gate's to write, not this change's**

## 2. The substrate on a device — `cy/field.slang` and the three bands

The one piece of work three gaps point at. `m10:fields-sampled-on-a-device` measures **zero**: there
are 26 `.slang` modules in the tree, 15 of them under `src/rendering/shaders/cy/` — `brdf`,
`cluster`, `color`, `frame`, `fullscreen`, `globals`, `light`, `material`, `noise`, `packing`,
`particle`, `sampling`, `shadow`, `tonemap`, `view` — and not one of them samples an environment
field.

- [ ] 2.1 **`cy/field.slang`**, written against the buffer layout `src/environment/include/cy/environment/gpu.h`
      already fixes and tests on the CPU, bound through the GPU scene as bindless resources so a
      shader samples a field without a per-draw binding. `src/environment/`'s README names this as
      owed by "the renderer-facing row that first samples a field in a shader", and no renderer-facing
      row has
- [ ] 2.2 **The agreement is measured on a device, not asserted.** `environment-fields`' *CPU and GPU
      access* requirement is half-discharged today — the README says so in as many words — and the
      half that is missing is the device. The criterion carries `requires = "gpu"` with a device-free
      companion, the shape `m10:world-still` and `integration.vfx` already use
- [ ] 2.3 **Band one: the substrate re-sampled at every terrain vertex, 63.0 ms.** Section 0 measured
      it; this is the shipping version of what section 0 prototyped
- [ ] 2.4 **Band two: the cloud march, 23.2 ms** — the only band in the capture that knows the time
      of day (25.5 ms with the sun up against 21.7 ms with it down, in every one of six captures)
- [ ] 2.5 **Band three: water's foam field, 12.0 ms**
- [ ] 2.6 The gap says in as many words that it closes **when those shaders exist, not by optimising
      the CPU loop**. A CPU optimisation that reaches 16.7 ms closes nothing and SHALL NOT be
      recorded as closing it

## 3. The two defects in the field model

- [ ] 3.1 **`m10:sky-field-round-trip` — the sky's write path.** `CloudShadowField::update` reports
      writing tiles darker than 0.5 and `CloudShadowField::sample` returns the declared 1.0 at all
      twenty-five points inside `radius_metres`. Terrain's and water's suites both go red when their
      `publish()` is suppressed, so the substrate's write path is sound and this is sky-specific
- [ ] 3.2 **Restore the two parked assertions as the check.** They are commented out in
      `src/rendering/sky/tests/test_cloud_shadows.cpp` with a note saying they fail today:
      `CY_CHECK_LT(lowest, 0.99F)` and `CY_CHECK_GT(highest - lowest, 0.01F)`. Restoring them is what
      closes the gap; the ledger's `run` reads the `through the store:` line either way, so a case
      that is renamed or skipped cannot look like a case that passed
- [ ] 3.3 **And the consumers.** `atmosphere-sky-and-clouds`' *Cloud shadows* requires the field to
      be "consumed by terrain, foliage, water, and illumination" and **nothing outside
      `src/rendering/sky/` reads it**. A standard field with a producer and no consumer is a field
      nobody has proved is readable — which is the same defect one level up
- [ ] 3.4 **`m10:fields-one-vegetation-potential` — a modelling decision before it is a code change.**
      `cy::foliage`'s `vegetation_potential_declaration()` declares `vegetation-potential`
      Scalar/UNorm8/Static/`SimulationClass::Persistent`; `cy::weather`'s `fields.h` declares the
      same name UNorm16/SlowlyVarying/Authoritative, and `FieldRegistry::declare()` refuses the
      second in either order, so a project registering both producers **fails at startup**
- [ ] 3.5 **Take the decision the specifications already imply, and record it.** `weather-and-wind`'s
      *Ecosystem state* gives macro vegetation density to weather; `environment-fields`' *Potential
      and current state* requires potential and current state to be **distinct fields**. So
      `vegetation-potential` is the ecosystem's, foliage's realised `vegetation` is the current state,
      and foliage consumes the potential rather than declaring it. One of the two declarations goes
      away; `specs/foliage/` and `specs/environment-fields/` in this change carry the decision
- [ ] 3.6 **`integration.standard_fields` goes red on purpose the day this lands**, because it
      asserts the known state. Update it in the same commit and say in its own text that the red was
      the answer arriving rather than a regression

## 4. The one continuous-integration job that answers three criteria

- [ ] 4.1 **A job that publishes one leg's digest and compares it with another's.** `ci.yml`'s build
      and test matrices already carry `linux-arm64`, `macos-arm64` and `windows-arm64`, and they run
      independently: nothing uploads a digest and nothing downloads one to compare. That upload-then-
      compare job is the only shape that answers any of the three
- [ ] 4.2 `m9:lockstep-cross-platform` — a simulation state hash compared between two architectures.
      It is a **declared gap** rather than a `where = "ci"` criterion precisely because `where = "ci"`
      would have let a single-leg suite satisfy it, which is the defect the practice exists to catch
- [ ] 4.3 `m10:pcg-regeneration-cross-platform` — a generated region's digest compared between two
      architectures. **NOT EVALUATED today**, and NOT EVALUATED is never a pass
- [ ] 4.4 `m10:pcg-gpu-domain-agreement` — the GPU execution domain against the CPU domain on more
      than one vendor's driver. Also NOT EVALUATED today
- [ ] 4.5 **A disagreement is a finding this rung reports, not a failure it hides.** The job exists to
      be capable of going red, and the honest outcome of three legs that do not agree is three legs
      that do not agree, recorded with the workloads and the digests

## 5. The four rows whose Working tier is claimed by a column and checked by nothing

`m9:record-matches-plan-history`, re-declared at M10's gate after task 6.5 closed it by recording the
four rows and the gate put the record back. The reason it went back is exact: over all fifteen
ledgers the only `expect_tiers` entry naming any of the four is `m0:roadmap-tiers` expecting `seed`,
and `_check_tiers` treats an exit tier as a **floor**, so `seed` can never contradict a Working claim.
`build-system-and-platforms` appears in no ledger but `m0.toml` at all.

- [ ] 5.1 **Write criteria that EVALUATE the rows, not tiers that record them.** One per row, each
      running something that can fail: `testing-and-quality` (M3), `build-system-and-platforms` (M4),
      `developer-workflow-and-just` (M6), `thirdparty-dependencies` (M8.b). The gap names two
      deliberate acts and this is the first of them
- [ ] 5.2 Each criterion checks the **evidence the matrix already writes out per row** —
      `capability-matrix.md` has the per-row argument for all four — rather than re-deriving it: the
      taxonomy's suites and the committed benchmark baseline; the four profiles, the feature options
      and the manifest-driven dependency fetch; every category of the required recipe surface doing
      something rather than refusing; the manifest's field set over every dependency plus the
      generated `THIRD_PARTY.md`
- [ ] 5.3 **Prove each one red.** A criterion over a row nobody has ever checked is exactly the shape
      that passes 44 of 44 with its subject deleted, which M9's gate found. Break what each checks
      and watch it fail, and record the demonstration with the change
- [ ] 5.4 **The tier cells stay where they are; this rung does not write them.** Recording a tier is a
      closing gate's act. The four rows' **Complete** cells remain at M11.d and M11.e, where the work
      that earns them is scoped; what this rung owes is that their **Working** tier stops being an
      unexamined claim
- [ ] 5.5 The `known_gap` declaration is deleted from `m9.toml` **only** when the four criteria run
      green and the four rows are recorded, in the change that does both — a declared gap that starts
      passing fails the ledger

## 6. `save-and-persistence`, re-scoped rather than demoted a third time

M9 demoted it naming two blockers; M10 demoted it a second time and audited it requirement by
requirement: **nine satisfied, three unmet, eight partial**, the table in `src/save/README.md`. M10's
`design.md` §4 wrote down in advance that a second demotion means the row is **mis-scoped rather than
late** and belongs in the next scoping pass. This is that pass.

- [ ] 6.1 **Confidentiality comes out of this rung and becomes its own change.** Requirement 15 is the
      only one of the eleven pieces that is a **dependency adoption**: `thirdparty-dependencies` names
      mbedTLS as this engine's cryptography library and requires adoption to go "through the OpenSpec
      change flow recording the evaluation against these criteria", with a key-management story
      attached. Carving it out with a named re-entry point is a decision recorded here, not an
      omission — `specs/save-and-persistence/` carries it
- [ ] 6.2 **The engine gets the consumer that lives in a sample.** The only translation between
      `world::PersistenceOverlay` and `save::Overlay` in the tree is `Session::to_save_overlay` inside
      `samples/06-open-world/`, and **no module above `src/save/` links `cy::save`**. Requirements 1
      and 12 are both that fact
- [ ] 6.3 **The save benchmark the requirement says the engine SHALL maintain.** `benchmarks/` holds
      `ecs`, `gameplay` and `micro` and no save entry (requirement 19), against a `benchmarks/`
      convention that already carries committed thresholds and tolerances
- [ ] 6.4 **The ten forbidden save patterns, "each SHALL be checkable"** — and today none is checked
      by any tool, criterion or grep (requirement 20). A pattern that is a review is not a check
- [ ] 6.5 **The inspector and the semantic diff** (requirement 18): nothing answers "why is this field
      in the save", and `Manifest::chunk_bytes_in` is one number one of them would need
- [ ] 6.6 **`LoadFailure::UnresolvableReference` is declared and produced by nothing**, and the
      scenario "references into unloaded regions" has no test (requirement 4)
- [ ] 6.7 The remaining partials — dirty flags `SaveService` captures and does not clear (6), the
      snapshot mechanism the specification names against what `service.cpp` does (9), per-record
      plugin ownership (14), checkpoints in memory *as well as* on storage (17)
- [ ] 6.8 **If this row is demoted a third time it is split, not deferred.** design.md §5 says so
      before the gate does, and the split is a change against `save-and-persistence`'s own
      specification rather than a fourth line in a gate report

## 7. The rest of this rung's rows

- [ ] 7.1 **`world-partition-and-streaming` → Complete, judged on a world that streams.**
      `samples/10-world` keeps the whole world resident, meshes every terrain tile at level 0 and
      reports `MeshReport::stitched_vertices` as zero precisely so a reader can see it. The streaming
      binder `src/terrain/`'s README records as its largest gap — `world::Channel::Terrain` exists,
      `cell_tile_footprint()` exists, and nothing pages tiles through `residency::ResidencyServer`
      the way `environment::FieldStreaming` pages field tiles — is the work
- [ ] 7.2 **`audio` → Complete, or a recorded deferral with its re-entry point.**
      `m8c:steam-audio-configures`: `-D CY_AUDIO_STEAM_AUDIO=ON` does not configure and
      `SteamAudioBackend::simulate` returns `NotImplemented`. The cost is **measured** in
      `deps/manifest.toml` rather than guessed — four upstream dependencies (pffft, zlib, libmysofa,
      flatbuffers with `flatc`) and a `-fabi-version=6` patch that breaks GCC — and the fetch and
      build of upstream have never been run. **A Complete cell over a backend that returns
      `NotImplemented` is the outcome this rung refuses**
- [ ] 7.3 **`terrain`, `water`, `foliage`, `weather-and-wind`, `environment-fields` →
      Complete.** Large but well understood once section 2 lands, and each blocked today by a
      criterion that is already red rather than by work nobody has started
- [ ] 7.4 **`procedural-content-generation` → Complete.** Its two NOT EVALUATED criteria are
      section 4's; what remains of the row is its own — the hundred-kilometre standing benchmark
      `src/pcg/README.md` records as a declared gap of the row, the derived-data cache bound to
      `build-and-packaging`'s artefact store, and the per-stage parallelism `classify_parallelism()`
      reports and the evaluator does not dispatch
- [ ] 7.5 **`simulation-and-determinism`, `replay-and-rollback`, `networking-and-replication` →
      Complete**, 67 requirements between them. Nothing has refused them because nothing has read
      them end to end at Complete grade — design.md §5 names that as a different kind of risk and
      asks for the read-through **before** the work is estimated, not after

## 8. The artefact — `samples/10-world`, inside its budget and streaming

Not a new sample. The honest artefact of a rung that pays debts is **the same world, measured the
same way, with the numbers moved**.

- [ ] 8.1 The same seed, the same capture, the same `--budget <csv>`: **mean and worst frame inside
      16.7 ms**, measured as a curve across a full day/night cycle rather than asserted at one time of
      day. Today it is about 122 ms mean and about 145 ms worst, nearly flat, over six captures
      spreading 121.6–123.9 ms
- [ ] 8.2 **The substrate sampled in a shader**, so the picture and the budget are produced by the
      same path the gap names
- [ ] 8.3 **The world streams**: tiles resident by cell rather than the whole world at level 0, and
      `stitched_vertices` no longer zero
- [ ] 8.4 **The seven gaps reported one by one, each closed or still red with its reason.** The
      artefact of this rung is the ledger as much as the picture
- [ ] 8.5 **Capture it.** `just capture-world` already writes `docs/design/images/m10-world.png`,
      `docs/design/images/m10-world-budget.png` and `docs/design/videos/m10-world.mp4` from nothing
      but a seed; the budget plot is the one that has to look different
- [ ] 8.6 A recorded gap exits non-zero and the headline figure reproduces, as M8.a's artefact rule
      requires of every sample

## 9. Records and gates

- [ ] 9.1 Write `tools/roadmap/milestones/m11a.toml` — M11.a's own criteria only; the ledger is flat.
      Every criterion carries a `ci_job` and must be able to fail
- [ ] 9.2 Declare `milestone-m11a` in `gates.toml` and raise `selftest.MINIMUM_CRITERIA`
- [ ] 9.3 An `m11b-open` criterion using the double-star glob form, so the ladder continues as a
      deliberate act
- [ ] 9.4 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md`, `dependencies.md` and
      `risks.md` — the last of these because M11's rungs have no spike recorded in the register and
      section 0 is this one's. Run the plan-consistency checks over all of them
- [ ] 9.5 Delete each closed gap's declaration from the ledger that holds it, in the change that
      closes it, and **leave every gap that is still open declared** — re-pointed, not removed
- [ ] 9.6 Move `ci.yml`'s milestone job to `m11a` in the same commit that flips the gate green. The
      job runs on every push to `main`, so this must not land before the gate is green
- [ ] 9.7 Open the M11.b change — its `tasks.md`, `design.md` and `specs/` are M11.b's own; what this
      rung owes is that the directory exists and the criterion can see it

## 10. The gate

- [ ] 10.1 Clean build of every profile from empty; `test-all` in each; every gate by hand
- [ ] 10.2 **Every criterion executes something and can fail** — break what it checks and prove it
      goes red. Sections 5.3 and 2.2 are the two most likely to pass over nothing
- [ ] 10.3 Adversarial pass on this rung's own invariants: a project registering both vegetation
      producers; the sky field read by a consumer outside `src/rendering/sky/`; the device sampler
      against the CPU sampler at the same positions; the budget across a full cycle at a seed the
      tuning did not use
- [ ] 10.4 **The seven gaps, one at a time.** Each either has its declaration deleted with its
      criterion green, or is re-declared with the rung that now closes it and the reason it did not
      close here. A false green on any one of the seven is the one outcome this rung cannot survive
- [ ] 10.5 Records verified against what the code supports, including this rung's own tier cells
      against the status record — `m9:record-matches-plan` is not milestone-specific in shape
