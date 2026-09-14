# Tasks: M11.a — Foundations

Ordered. Section 0 is the spike and it runs first because **the whole of this rung's estimate rests
on one unmeasured assumption** — that the 122 ms frame is a shader problem. Section 1 is the split,
because the seven inherited gaps cannot be re-pointed at a rung the ladder does not contain, and
every section below it is a gap that is failing today.

Nothing in sections 2 to 7 is new scope. Each is a criterion that already runs, already fails, and
already names M11 as the rung that closes it, or a row whose Complete cell is blocked by one.

## 0. The spike — one band on a device, before the other two are scoped

- [x] 0.1 **Measure, do not assume: port the 63.0 ms band and time it.** The substrate re-sampled at
      every terrain vertex is the largest of the three bands `m10:world-frame-budget` names, and it
      is the one whose shape a GPU field sampler most directly changes. Sample the same field, at the
      same positions, at the same resolution, on the device, against the CPU path, and report **the
      band's cost and the agreement of the two answers**. A sampler that is fast and disagrees is not
      a result
- [x] 0.2 **What the spike must refuse to report.** M10's spike caught its own meter measuring
      nothing and exited non-zero rather than printing a matrix; do the same here. A run where the
      device path never executed, where the comparison sampled outside the written tiles, or where
      the two paths agree because both returned the declared default, SHALL exit non-zero saying so
      rather than reporting a speed-up
- [x] 0.3 State plainly what the spike cannot answer on this host, through the ledger's own
      `requires`/`where` mechanism rather than as a sentence. One GPU vendor is one driver's
      floating-point behaviour, and `m10:pcg-gpu-domain-agreement` is already the precedent for
      refusing that claim here
- [x] 0.4 Commit the spike outside the repository — `~/cyberdyne-spikes/`, as M3's, M5.5's, M6's,
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

- [x] 2.1 **`cy/field.slang`**, written against the buffer layout
      `src/environment/include/cy/environment/gpu.h` already fixes and tests on the CPU, bindless
      through the GPU scene at set 0 binding 3 — `cyFieldSampleScene(slot, x, y, z)` — so a shader
      samples a field without a per-draw binding. **Two consumers import it**, which is what keeps it
      from being a file: `cy/terrain_shade.slang` (the substrate half of the 63.0 ms band, the colour
      arithmetic expression for expression from `World::shade_terrain()`) and `cy/cloud_shadow.slang`
      (the one line the four consumers `atmosphere-sky-and-clouds` names all want). All three are
      compiled by `smoke.material_slang`'s last case through the engine's own Slang front end, so a
      module nothing compiles cannot satisfy the grep-shaped half of the gap
- [x] 2.2 **The agreement is measured on a device, BIT-EXACTLY.** `render.environment_field` —
      tests/render/, layer 7, because `cy::environment` is layer 2 and may not name a device.
      17 152 comparisons over two fields: a quantised scalar and a **volumetric** Vec3/F32 with four
      vertical cells, which is the case design.md §1.4 said the spike could not test. At lattice
      centres the F32 field is bit-identical 6912 of 6912; the quantised one lands within **one** ULP,
      which is Vulkan specifying `OpFDiv` to 2.5 ULP rather than a disagreement — measured, and
      `precise` on the decode was tried and changed nothing. Across a sweep the worst is 3 ULP
      (scalar) and 19 (volumetric, near zero, 9.06e-06 against a declared precision of 8e-4).
      **Five mutations run against the finished suite**, four red, and the fifth recorded because it
      was not: `/255`→`/256` (worst absolute 0.00383735 — INSIDE the declared precision, so the
      specification's own bound could not have caught it), the half-cell offset, the dispatch
      removed, the comparison moved 1 000 km, and `wx*wz*wy`→`wx*(wz*wy)`, which MOVED the volumetric
      count and failed nothing
- [ ] 2.3 **Band one: the substrate re-sampled at every terrain vertex, 63.0 ms.** Section 0 measured
      it; this is the shipping version of what section 0 prototyped
- [ ] 2.4 **Band two: the cloud march, 23.2 ms** — the only band in the capture that knows the time
      of day (25.5 ms with the sun up against 21.7 ms with it down, in every one of six captures)
- [ ] 2.5 **Band three: water's foam field, 12.0 ms**
- [ ] 2.6 The gap says in as many words that it closes **when those shaders exist, not by optimising
      the CPU loop**. A CPU optimisation that reaches 16.7 ms closes nothing and SHALL NOT be
      recorded as closing it

## 3. The two defects in the field model

- [x] 3.1 **`m10:sky-field-round-trip` — NOT the sky's write path.** The gap's text said `update`
      wrote tiles `sample` could not read back, and the write path is sound: reading every cell the
      producer writes back through `FieldStore::sample_at()` resolves **1024 of 1024 regional cells
      and 256 of 256 macro cells**, darkest 0.0039 against a producer report of 0.0041 — one
      `UNorm8` quantum. What was wrong was **the test's sampling positions**: a 5x5 grid 256 m about
      the origin, over ground genuinely in full sun under that weather (the cloud map's cells are
      1000 m and the mean over everything written is 0.974). Twenty-five samples of lit ground read
      the declared 1.0 whether the field was published or not, which is why suppressing `publish()`
      did not move them — the round trip was measured where nothing was written, which is the same
      defect the gap describes, one level up. `src/rendering/sky/README.md` carries the finding
- [x] 3.2 **Restored, and made a round trip rather than a spot check.** The two assertions are back
      and pass — `through the store: 2048 samples, lowest 0.00392157, highest 1` — and they are now
      measured over **every cell the producer wrote at both levels**, with the extremes compared
      against `stats()` to within one `UNorm8` quantum. Proved failing twice: `publish()` suppressed
      gives 0 resolved cells and a lowest of 1; values written 10% dark moves the highest to 0.9020
      and the quantum comparison catches it. `m11a:sky-field-round-trips` named `unit.render_sky`,
      which does not contain this file at all — a criterion that greps one suite and runs another
      cannot fail on the defect it exists for — and now runs `integration.render_sky_fields` and
      reads its numbers
- [ ] 3.3 **And the consumers.** `atmosphere-sky-and-clouds`' *Cloud shadows* requires the field to
      be "consumed by terrain, foliage, water, and illumination" and **nothing outside
      `src/rendering/sky/` reads it**. A standard field with a producer and no consumer is a field
      nobody has proved is readable — which is the same defect one level up
- [x] 3.4 **`m10:fields-one-vegetation-potential` — a modelling decision before it is a code change.**
      `cy::foliage`'s `vegetation_potential_declaration()` declares `vegetation-potential`
      Scalar/UNorm8/Static/`SimulationClass::Persistent`; `cy::weather`'s `fields.h` declares the
      same name UNorm16/SlowlyVarying/Authoritative, and `FieldRegistry::declare()` refuses the
      second in either order, so a project registering both producers **fails at startup**
- [x] 3.5 **Take the decision the specifications already imply, and record it.** `weather-and-wind`'s
      *Ecosystem state* gives macro vegetation density to weather; `environment-fields`' *Potential
      and current state* requires potential and current state to be **distinct fields**. So
      `vegetation-potential` is the ecosystem's, foliage's realised `vegetation` is the current state,
      and foliage consumes the potential rather than declaring it. One of the two declarations goes
      away; `specs/foliage/` and `specs/environment-fields/` in this change carry the decision
- [x] 3.6 **`integration.standard_fields` goes red on purpose the day this lands**, because it
      asserts the known state. Update it in the same commit and say in its own text that the red was
      the answer arriving rather than a regression

## 4. The one continuous-integration job that answers three criteria

- [x] 4.1 **A job that publishes one leg's digest and compares it with another's.** `ci.yml`'s build
      and test matrices already carry `linux-arm64`, `macos-arm64` and `windows-arm64`, and they run
      independently: nothing uploads a digest and nothing downloads one to compare. That upload-then-
      compare job is the only shape that answers any of the three.
      **Built as `cross-leg-publish` (four legs, two architectures, three operating systems) and
      `cross-leg-compare` in `ci.yml`.** The publisher is `determinism.cross_leg`
      (`tests/determinism/test_cross_leg.cpp`), which computes the digest out of the engine's own
      pieces — the golden session of `src/replay/tests/sim.h` and the forest graph of
      `src/pcg/tests/fixtures.h` — and checks what it publishes; the comparator is
      `tools/ci/cross_leg_digests.py`; both halves are modes of one recipe,
      `just test-determinism --publish-digest <path>` and `just test-determinism --compare-legs [--pcg]`
- [x] 4.2 `m9:lockstep-cross-platform` — a simulation state hash compared between two architectures.
      It is a **declared gap** rather than a `where = "ci"` criterion precisely because `where = "ci"`
      would have let a single-leg suite satisfy it, which is the defect the practice exists to catch.
      **Its criterion now passes and its `known_gap` declaration is deleted from `m9.toml`**, because
      a declared gap that starts passing fails the ledger. What the deletion claims is that the
      comparison EXISTS; whether the two architectures AGREE is `m11a:lockstep-agrees-across-architectures`,
      which runs the comparator, carries `where = "ci"` and is NOT EVALUATED on this one-architecture
      host
- [x] 4.3 `m10:pcg-regeneration-cross-platform` — a generated region's digest compared between two
      architectures. **NOT EVALUATED today**, and NOT EVALUATED is never a pass. The same job answers
      it: the comparator's `--pcg` claim compares `pcg-world-digest` and `pcg-identity-digest`, the
      two numbers `src/pcg/tests/test_determinism.cpp` already compares between two runs on ONE host
- [x] 4.4 `m10:pcg-gpu-domain-agreement` — the GPU execution domain against the CPU domain on more
      than one vendor's driver. Also NOT EVALUATED today. **THIS ONE IS NOT CLOSED AND THE JOB
      DELIBERATELY DOES NOT ANSWER IT.** Two things are absent and only one is a runner question:
      `cy::pcg::ExecutionDomain` is Editor, Cook, Runtime, Streaming, Dynamic — there is no GPU
      execution domain in this tree to compare — and no hosted runner in `ci.yml` has a device.
      `--gpu-domain` prints every leg's field and refuses. **Its check was strengthened here because
      the new job turned it green by mentioning the subject**: it asked only that some job block
      contain `gpu`, a `pcg` word and `download-artifact`, which `cross-leg-compare`'s own comment
      explaining why it does not answer the question satisfied. It now requires the job to NAME TWO
      GPU VENDORS, and it is re-declared in `m10.toml` as a `known_gap` closing at **M11.e**, the
      rung that owns the full matrix. `determinism.cross_leg`'s last case asserts that no execution
      domain names a device, so the first half goes red the day a GPU domain is added
- [x] 4.5 **A disagreement is a finding this rung reports, not a failure it hides.** The job exists to
      be capable of going red, and the honest outcome of three legs that do not agree is three legs
      that do not agree, recorded with the workloads and the digests. The comparator exits **1** on a
      disagreement, printing every leg's number beside its architecture, and **2** on a comparison it
      could not make — one leg, one architecture, a zero digest, an empty workload, an unreadable
      schema — which must never be read as a pass. Fifteen negative fixtures in
      `tools/ci/test_cross_leg_digests.py` hold that, and they run under `just ci-check`

## 5. The four rows whose Working tier is claimed by a column and checked by nothing

`m9:record-matches-plan-history`, re-declared at M10's gate after task 6.5 closed it by recording the
four rows and the gate put the record back. The reason it went back is exact: over all fifteen
ledgers the only `expect_tiers` entry naming any of the four is `m0:roadmap-tiers` expecting `seed`,
and `_check_tiers` treats an exit tier as a **floor**, so `seed` can never contradict a Working claim.
`build-system-and-platforms` appears in no ledger but `m0.toml` at all.

- [x] 5.1 **Write criteria that EVALUATE the rows, not tiers that record them.** One per row, each
      running something that can fail: `testing-and-quality` (M3), `build-system-and-platforms` (M4),
      `developer-workflow-and-just` (M6), `thirdparty-dependencies` (M8.b). The gap names two
      deliberate acts and this is the first of them.
      **Done** — `m11a:testing-and-quality-at-working`, `m11a:build-system-at-working`,
      `m11a:developer-workflow-at-working` and `m11a:thirdparty-dependencies-at-working`, over
      `tools/roadmap/row_evidence.py`. **And the regression guard that was supposed to protect them
      could not fail**: `the-four-rows-are-evaluated` globbed `milestones/*.toml`, so
      `build-system-and-platforms` was satisfied by five criteria in `m11d.toml` and `m11e.toml` and
      `thirdparty-dependencies` by two — none of which `criteria.build_plan` puts in M11.a's plan —
      and its floors came from `roadmap-tiers` entries two and three rungs after the tier is
      recorded. It now reads the plan rather than the directory, and deleting the four criteria above
      takes it red (it did not before; both runs are in the demonstration under 5.3)
- [x] 5.2 Each criterion checks the **evidence the matrix already writes out per row** —
      `capability-matrix.md` has the per-row argument for all four — rather than re-deriving it: the
      taxonomy's suites and the committed benchmark baseline; the four profiles, the feature options
      and the manifest-driven dependency fetch; every category of the required recipe surface doing
      something rather than refusing; the manifest's field set over every dependency plus the
      generated `THIRD_PARTY.md`.
      **Done, and the first thing an evaluating criterion found is that one quarter of one row's
      stated evidence is false**: the matrix argues `developer-workflow-and-just` at Working on
      *"Every category of the required recipe surface does something from M6: … Maintenance,
      Release"*, and all four Release recipes are `_not-implemented` stubs and have been since M0.
      The criterion is declared as a gap closing at M11.d, where `release-recipes-stop-refusing`
      already is. The other three rows' evidence holds in full — including
      `tools/deps/test_gating.py`, which the `thirdparty-dependencies` row has named since M8.b and
      which **no criterion and no CI job has ever run**
- [x] 5.3 **Prove each one red.** A criterion over a row nobody has ever checked is exactly the shape
      that passes 44 of 44 with its subject deleted, which M9's gate found. Break what each checks
      and watch it fail, and record the demonstration with the change.
      **Twenty-six mutations, every one of them verified live and every source md5-restored** —
      `~/cyberdyne-spikes/m11a-row-evidence/MUTATIONS.txt`. They include the two directions the
      declared gap needs: the Release category made to stop refusing takes
      `developer-workflow-at-working` GREEN, so the gap is a deadline rather than a permanent red.
      **One mutation of this pass was itself defective and is recorded beside its fix**: the first
      attempt at "a second whole category refuses" left `just/content.just` unparseable, so the
      criterion went red because `just --summary` failed rather than because the category refused —
      a mutation that kills the tool proves nothing about the subject
- [x] 5.4 **The tier cells stay where they are; this rung does not write them.** Recording a tier is a
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

- [x] 6.1 **Confidentiality comes out of this rung and becomes its own change.** Requirement 15 is the
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
- [x] 6.8 **If this row is demoted a third time it is split, not deferred.** design.md §5 says so
      before the gate does, and the split is a change against `save-and-persistence`'s own
      specification rather than a fourth line in a gate report.
      **The scoping pass ran and it did not find a split**, and that answer is recorded in
      `proposal.md` rather than left for the gate. Read against this tree, the row divides into a
      **carve-out** and a **correction**, not two capabilities. The carve-out is 6.1. The correction
      is requirement 14: `src/save/README.md` says per-record plugin ownership *"needs the module
      registry that arrives with `project-and-plugins`"*, and **that claim is stale** —
      `identity/manifest.toml` already carries a `module` for every type it has issued an identifier
      to, and `cy::config::ModuleRegistry` exists besides, so the work is ordinary engineering rather
      than a wait on another capability. `specs/save-and-persistence/` now says the owning module is
      READ FROM the identity manifest rather than re-derived, because a second type-to-module table
      inside the save container would fork the identity record. Everything else left open is ordinary
      engineering inside or immediately above `src/save/`; moving those requirements into a second
      document would rename the debt without moving one line of work. **The row was mis-scoped in a
      different way than "too big"**: two milestones planned it Complete on two named blockers and
      nobody read it requirement by requirement until M10's close found eleven pieces. M10's audit
      re-verified here against today's tree — nothing above `src/save/` links `cy::save`, there is no
      `benchmarks/save`, no `tools/save/`, and `LoadFailure::UnresolvableReference` is still declared
      and produced by nothing — so all twenty rows of that table still hold. **A third demotion would
      now mean something new**, because the bill is itemised; if it happens, the split this task
      reserves is still the answer and the evidence for it would be different evidence

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
      `NotImplemented` is the outcome this rung refuses**.
      **NOT CLOSED, AND RE-MEASURED RATHER THAN RE-QUOTED.** On this host on 2026-09-14
      `-D CY_AUDIO_STEAM_AUDIO=ON` configures the engine, fetches `steam_audio` at its pin and fails
      at upstream's `core/CMakeLists.txt:235`, `find_package(PFFFT)`; `-fabi-version=6` still makes
      libstdc++'s `<future>` fail under GCC 13.3.0 and is still an unknown argument to clang 18.1.3,
      and without it the same g++ line exits 0. `deps/manifest.toml` now carries the re-measurement
      and the FIVE things closure changes — four `[[dependency]]` entries each needing its own
      recorded evaluation, a manifest field for a build-time tool (`flatc` is built and then run), a
      patch mechanism `cmake/dependencies.cmake` does not have, cache-variable plumbing every other
      dependency would share a configure with, and the backend body plus a
      `unit.audio_acoustics` case named "steam audio simulates" for `m11a:steam-audio-simulates` to
      select. **The gate's decision to take: `m8c:steam-audio-configures` is re-declared with a rung
      that owns a dependency adoption — M11.e, beside `dependency-set-integrated` — or this rung
      spends the morning the manifest says it costs. It is not closed here**
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
