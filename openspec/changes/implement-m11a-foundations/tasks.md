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
- [x] 2.7 **AND THE GAP'S OWN CHECK WAS A WORD-GREP — REPAIR 1, after this rung's gate refused the
      claim that the four M10 gaps were closed without weakening anything.**
      `m10:fields-sampled-on-a-device` asked
      `grep -rl "FieldImageHeader\|field_image\|kFieldImage" --include='*.slang' src/` and wanted a
      non-zero count; `m11a:field-sampler-written` greped the same three words in one file;
      `m11a:field-sampler-consumed` counted files containing `cy.field`. `falsify.py audit --weak`
      names all three `presence-only` — **the verdict is that this text exists** — so a `.slang` file
      whose whole content is a comment closed the gap, and the two consumers satisfied `consumed` by
      carrying an `import` line they never call. Nothing weakened those checks at this rung; they
      were already too weak to tell the shader that was written from the shader that was not.
      **They now run the shader's compiler.** `m10:fields-sampled-on-a-device` requires the module and
      its two consumers to TYPE-CHECK through the engine's own Slang front end inside
      `smoke.material_slang`'s probe, which CALLS `cyFieldSampleScene`, `cyTerrainSampleSubstrate`,
      `cyTerrainShade`, `cyCloudShadowAt` and `cyCloudShadowAttenuate`; and it recompiles
      `tests/render/shaders/field_probe.slang` and requires the result to equal
      `field_probe_spirv.h` word for word — **the only thing in the tree that ties
      `render.environment_field`'s ULP figures to a source file**, because that SPIR-V is checked in
      for builds with no compiler and until now editing `cy/field.slang` changed nothing any suite
      executed. `m11a:field-sampler-written` compares SIXTEEN layout constants — magic, version,
      header and entry words, tile edge, five `FieldEncoding` members, five `FieldLayerRule` members,
      `FieldInterpolation::Linear` — between the shader and `gpu.h`, `field.h` and `store.h`, which is
      what "written against the layout gpu.h fixes" actually asserts. `m11a:field-sampler-consumed`
      compiles ONE PROBE PER CONSUMER and requires the emitted SPIR-V to reference the bindless table
      at set 0 binding 3; the negative control is recorded rather than claimed — a probe calling
      `cyTerrainShade`, which takes an already-sampled substrate, compiles to SPIR-V with ZERO
      references to it.
      **Four reds, run and restored md5-identical.** Renaming `cyFieldSampleScene` in
      `cy/field.slang` (the word `kFieldImage` and both `import cy.field;` lines left exactly where
      the greps found them) takes `fields-sampled-on-a-device` and `field-sampler-consumed` red;
      reassociating line 486's `wx * wz * wy` to `wy * wz * wx` still COMPILES and takes the SPIR-V
      half red with both embedded modules DIFFERENT; `kFieldImageVersion = 2u` takes
      `field-sampler-written` red; and replacing `cyTerrainSampleSubstrate`'s four samples with
      constants leaves `terrain_shade.slang` compiling, leaves its `import` line in place, and takes
      `field-sampler-consumed` red at **1 of 2 consumers** — the defect the old grep counted as two.
      `field-sampler-written` is now PROVEN by `just roadmap-falsify` itself; the other two are
      recorded `not provable here` because the prover's sandbox is the tracked tree and they need a
      build, and each declares the `[criterion.falsifies]` a prover with a build would apply

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
- [x] 4.6 **AND THE THREE CRITERIA WERE WORD-GREPS THE WHOLE TIME — REPAIR 1, after this rung's gate
      refuted the claim.** The job above is real and its comparator's refusals are tested, and none
      of that reached the three criteria: each searched `.github/workflows/*.yml` for a job block
      carrying `download-artifact` beside `digest`, `lockstep` or a `pcg` word. A six-line job that
      uploads nothing, downloads nothing and runs `echo "lockstep and pcg digests compared"` turns
      all three green — measured, on this tree, against the checks as they stood. So deleting the
      `run:` line from `cross-leg-compare` would have left every one of them green, which is the
      eighth unfalsifiable check this ladder has found and the reason `just roadmap-falsify` exists.
      **`tools/ci/cross_leg_audit.py` replaces the search with the job's own behaviour**: it reads
      the workflow only as far as the publisher, the downloader and the command between them, and
      then EXECUTES that command against digest files it writes — two architectures that agree must
      pass it, and a changed simulation hash, a changed generated world, one leg alone, two legs of
      one architecture, a zero digest and an empty workload must each turn it red. `m9`'s criterion
      runs `--claim lockstep`, `m10`'s runs `--claim pcg` (and gives up its `where = "ci"`, because
      what it asks now is answerable on any host), `m11a`'s runs `--claim job`. Six workflows it must
      refuse — the dummy first — are `--selftest` fixtures under `just ci-check`, and all three
      criteria declare a `[criterion.falsifies]` the tooling executes: delete the `--compare-legs`
      line, rename `--pcg`, delete the `download-artifact` lines. All three are PROVEN in
      `tools/roadmap/falsifiability.toml`, which is three entries shorter than it was
- [x] 4.7 **AND A COMMAND IS NOT A JOB — REPAIR 2, after the gate refuted repair 1.** The comparison
      COMMAND was genuinely defended by 4.6; the JOB was not. The gate broke `.github/workflows/ci.yml`
      three ways in a sandbox and all three criteria stayed GREEN on every one: `if: always()` changed
      to `if: false` on `cross-leg-compare`, so nothing is ever compared **and GitHub scores a skipped
      job as success, leaving the whole pipeline green**; the matrix leg's
      `Publish this leg's simulation and generation digests` step deleted verbatim, so four legs upload
      a path nothing computed; and the same step's command replaced by an `echo` or its flag misspelt,
      so the leg runs, exits and routes nothing to the path it uploads. A command that discriminates,
      inside a job that never runs, over an artefact nobody wrote, is the dummy job again with more
      lines in it. **`tools/ci/cross_leg_audit.py` now answers two more questions and both fail
      CLOSED.** *Is the comparison scheduled?* — a reader for the subset of GitHub's expression
      language a job's `if:` uses (`always()`, `success()`, `failure()`, `cancelled()`,
      `github.event_name`, `github.ref`, literals, `==`, `!=`, `!`, `&&`, `||`, parentheses),
      evaluated over the workflow's OWN `on:` triggers against the publishing job, the comparison job,
      the `needs:` chain between them and each step the chain is made of; a condition it cannot read is
      reported as a finding, because "I could not tell whether it runs" must never print the same as
      "it runs". *Does a leg publish anything?* — the leg's own publishing command is RUN, with the
      path it uploads redirected into a directory the check has deliberately not created and with
      `CY_BUILD_DIR` pointed inside a regular file so no build can succeed: the real command creates
      that directory in 60 ms before it dies at the build, an `echo` exits 0 having created nothing,
      and a misspelt flag dies having touched nothing at all. **Watched red, in a sandbox copy of the
      tree, `md5sum` 31543d5d4264e3638d4a6bc1a0cfcf03 restored after each**: all three of the gate's
      mutations now take all three criteria to exit 1, as do `if: false` on the publishing job, on the
      comparison step and on the upload step, and a job restricted to an event this workflow never
      fires. The twelve `--selftest` fixtures are built from ONE template so each is the working
      workflow with one thing changed, and **each names the finding it must provoke** — a rule that
      began refusing everything would otherwise read exactly like twelve discriminating ones.
      `m11a:cross-leg-digest-job`'s declared mutation moves off `download-artifact`, which broke the
      half that was already defended, onto `--publish-digest`, which is the half that was not; it is
      re-PROVEN in `tools/roadmap/falsifiability.toml` and m9's and m10's proofs still stand
- [x] 4.8 **AND A JOB IS NOT A RUN — REPAIR 3, after the gate refuted repair 2.** The gate added ONE
      LINE to `.github/workflows/ci.yml` — `continue-on-error: true` on the
      `One leg's digest against another's` step — and all three criteria stayed GREEN, as did
      `just ci-check`, which printed *"this repository's own workflows carry a comparison that
      discriminates"* over a comparison whose answer was being thrown away. The comparator still runs.
      It still exits 1 when two architectures disagree. The step is marked failed and **the job's
      conclusion is SUCCESS**, so the run is green and the disagreement is a grey tick nobody looks
      at. Every check written so far measured the comparison's COMMAND, then whether that command was
      REACHED; none asked whether the pipeline can SEE the answer, and red is a property of the RUN
      rather than of a process's exit code. **`tools/ci/cross_leg_audit.py` now walks the chain a
      third time asking DOES THE RED REACH THE PIPELINE, and it fails CLOSED like the other two.**
      Every `continue-on-error:` on the publishing job, on the comparison job and on each step the
      chain is made of — publish, upload, download, compare — evaluated with the same expression
      reader per trigger, so forgiveness granted only on pull requests is still forgiveness and is
      reported as such; and the `if-no-files-found:` of the step that uploads the digest, which at
      anything but `error` (the action's default is `warn`) lets a leg that computed nothing stay
      green and drop out of the comparison in silence while the legs that remain agree with each
      other. An expression the reader cannot evaluate is a finding, because "I could not tell whether
      a red would fail the run" must never print the same as "a red fails the run". **Watched red in a
      sandbox copy of the tracked tree, the repository's `.github/workflows/ci.yml` `md5sum`
      31543d5d4264e3638d4a6bc1a0cfcf03 before and after**: the gate's own one-liner, the same flag on
      the comparison JOB, the same flag on the publishing step, `if-no-files-found: error` deleted,
      and `continue-on-error: ${{ vars.CY_TOLERATE_DRIFT }}` each take all three criteria to exit 1,
      and the gate's one-liner takes `just ci-check` to exit 1 as well. Five more `--selftest`
      fixtures, seventeen in all, each still naming the finding it must provoke; **deleting the new
      check's call site turns `just ci-check` red with five fixtures ACCEPTED**, and `just ci-check`
      is a criterion of m5, m10, m11a and every M11 rung, so the check cannot be removed quietly. What
      it does not claim: that anyone has made the run REQUIRED for a merge — branch protection is a
      repository setting and lives in no file in this tree

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
      a mutation that kills the tool proves nothing about the subject.
      **REPAIR 2 — TWENTY-SIX MUTATIONS AND NOT ONE OF THEM BROKE A BEHAVIOUR.** M11's second repair
      round re-read what those twenty-six actually did and found the eighth instance of this
      project's one defect, in two of the four criteria above. `testing-and-quality-at-working` had
      three legs that were `fragment in taxonomy` — a substring search over `tests/CMakeLists.txt` —
      and a fourth that was `^  <job>:$` over `ci.yml`; `build-system-at-working` read
      `cmake/profiles.cmake` with regular expressions. Both were shown GREEN over a broken tree:
      `cy_add_test`'s `PRIVATE_DEFINITIONS`, `LABELS` and `TIMEOUT` lines commented out, with the
      words left behind in the comments, so no suite would carry a budget, a label or a timeout —
      green; all seven quality jobs replaced by `run: echo dummy` — green; every
      `set(CY_PROFILE...)` statement in `cmake/profiles.cmake` commented out, leaving a build system
      with no profile table at all — green on every profile leg. **The subject is handed to CMake
      now**: `tools/roadmap/probes/taxonomy/` configures a project that includes the taxonomy,
      declares one suite per kind CMake DEFINES, and reports the `CY_TEST_BUDGET_NS` `cy_add_test`
      passed to `cy_add_module` and the `LABELS`/`TIMEOUT` the generator wrote into
      `CTestTestfile.cmake`; `tools/roadmap/probes/profiles/` calls
      `cy_declare_build_configurations()` before `project()` exactly as the top-level CMakeLists.txt
      does, then `cy_profile_for_configuration()` and `cy_declare_features()`. The CI leg asks what
      each of the seven jobs RUNS against the live recipe surface. All three mutations above are RED
      now — the profiles one answering with the shipped macro's own `CY_PROFILE is 'debug', which is
      not a build profile` — and two more were added: the `CY_FEATURE_OPTIONS` table commented out
      (`cy_declare_features() declared nothing`) and the `_not-implemented` helper renamed away,
      which the `developer-workflow-and-just` guard leg now catches by RUNNING it rather than by
      spelling it. Every source md5-restored; `falsify.py prove m11a --only at-working` re-earns
      PROVEN for the first two and the declared-gap proof for the third
- [x] 5.4 **The tier cells stay where they are; this rung does not write them.** Recording a tier is a
      closing gate's act. The four rows' **Complete** cells remain at M11.d and M11.e, where the work
      that earns them is scoped; what this rung owes is that their **Working** tier stops being an
      unexamined claim
- [ ] 5.5 The `known_gap` declaration is deleted from `m9.toml` **only** when the four criteria run
      green and the four rows are recorded, in the change that does both — a declared gap that starts
      passing fails the ledger
- [x] 5.6 **AND THE GUARD OVER THE FOUR COULD NOT DETECT TWO OF THE FOUR DELETIONS — REPAIR 1**, in
      the gate's own words: `the-four-rows-are-evaluated` asked whether the row's name appeared in a
      criterion's `source`, and `source` is a **citation**. Eight criteria in M11.a's plan cite
      `testing-and-quality` because that specification governs them and seven cite
      `developer-workflow-and-just`, so deleting the criterion that actually evaluated either row
      left the bystanders answering for it. Reproduced against `HEAD` before repairing: deleting
      `testing-and-quality-at-working` and deleting `developer-workflow-at-working` each left the old
      guard **green**, and the other two took it red — two of four, exactly as the gate said.
      **The question now has a field to be asked of.** `criterion.evaluates` is declared by the
      criterion that does the evaluating and by nothing else (`criteria._check_evaluates`,
      `criteria.evaluators`, four selftest cases including the citation that must NOT count), and the
      guard reads it through `build_plan` so that a declaration in m11d is still not a check this
      rung runs. Deleting each of the four `[[criterion]]` tables whole now takes it **red**, all
      four verified, `m11a.toml` restored md5-identical. It also grew the ordering leg this gap is
      about: **no rung may expect a row above `seed` before the rung that evaluates it**, and the
      record's own tier may not be ahead of the evaluation either.
- [x] 5.7 **All four evaluators now carry a proof the tooling re-earns on every `just roadmap-test`**,
      where 5.3's demonstration was twenty-six mutations in a spike directory that nothing re-runs.
      `testing-and-quality-at-working` goes red with the per-case budgets deleted from the taxonomy;
      `build-system-at-working` with `set(CY_PROFILES` deleted from `cmake/profiles.cmake`;
      `the-four-rows-are-evaluated` with one evaluator's declaration deleted. `developer-workflow-at-working`
      is a **declared gap** and is judged the other way round — it is red on the unmutated tree, which
      is what a gap IS, so its mutation must make it **GREEN**: the Release category stops refusing
      and it passes, which is the difference between a deadline and a check that can never pass
      (`falsify._prove_a_declared_gap`, three selftest cases). `thirdparty-dependencies-at-working`
      cannot be judged by a source-only sandbox — its gating leg configures CMake twice and fetches,
      which is why its ledger asks for 7200 s — so a criterion whose declared budget is past the
      ledger default is routed to the build-backed proof, and it is recorded **red against a built
      tree**: watched failing in 5 m 39 s on `build/repair-1-2`, three legs holding and the gating leg
      short on `steam_audio`'s PFFFT. Two SOURCE defects were found and fixed on the way: `cmake_files()`
      crashed outside a git repository, which is where every proof runs, and `m11e`'s two record
      criteria called `record.read()`, which does not exist — the criterion that CLOSES this gap was
      failing on an AttributeError rather than on the four cells it exists to name

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
- [x] 10.2 **Every criterion executes something and can fail** — break what it checks and prove it
      goes red. Sections 5.3 and 2.2 are the two most likely to pass over nothing.
      DONE for all 37 of `m11a.toml`'s criteria, one `[[proof]]` each in
      `tools/roadmap/falsifiability.toml` and no `[[unproven]]` entry left for this ledger.
      REPAIR ROUND 2 closed the last two holes, and both were in the MECHANISM rather than in a
      criterion:
      (a) `lockstep-agrees-across-architectures` and `pcg-regenerates-across-architectures` carry
      `where = "ci"` — this host has one architecture — and `falsify.prove` refused to judge them at
      all, so nobody had ever watched them fail. They now declare `[criterion.ci_proof]`: the command
      that BUILDS what continuous integration hands them
      (`tools/ci/cross_leg_audit.py --write-legs`, the agreeing pair from the publisher's own field
      list), and the mutation of that environment. The prover runs the criterion's own body three
      times against it — green, RED with one leg's `sim-state-digest` / `pcg-world-digest` renamed,
      green again once restored — and records `proven in the environment CI supplies`. A `provide`
      that supplies nothing leaves the criterion red at its positive control and is `not provable
      here`, which was checked by breaking `--write-legs` and watching the verdict come back.
      (b) `network-at-complete-grade` ran `just quality-requirements`, which did not exist: `just`
      aborted at argument parsing, ran nothing, exited 1 — and "red unmutated in the sandbox and in
      the repository" is what `falsify._red_in_the_tree` records as a PROOF. The mechanism was
      counting a name that does not resolve as a check that had been watched going red. `falsify`'s
      new `absent-recipe` rule is in `CANNOT_GO_RED` and refuses the shape before any run (it finds
      eleven such criteria across the ladder, and none of the other 630), and the recipe those
      criteria were written for now exists: `tools/roadmap/requirements.py` reads each row's
      `### Requirement:` headings out of `openspec/specs/` and resolves each against a committed
      `cy_add_test()`, a gate id, a criterion id or a recorded exemption. The criterion is RED
      today — 0 of 67 requirements across its three rows map to anything — and that red is the
      finding, not a missing check
- [ ] 10.3 Adversarial pass on this rung's own invariants: a project registering both vegetation
      producers; the sky field read by a consumer outside `src/rendering/sky/`; the device sampler
      against the CPU sampler at the same positions; the budget across a full cycle at a seed the
      tuning did not use
- [ ] 10.4 **The seven gaps, one at a time.** Each either has its declaration deleted with its
      criterion green, or is re-declared with the rung that now closes it and the reason it did not
      close here. A false green on any one of the seven is the one outcome this rung cannot survive
- [ ] 10.5 Records verified against what the code supports, including this rung's own tier cells
      against the status record — `m9:record-matches-plan` is not milestone-specific in shape
- [x] 10.6 **The four M10 gaps, one at a time, with the third clause checked rather than asserted —
      REPAIR 1.** Not all seven: 10.4 stays open because `m8c:steam-audio-configures` is 7.2's and
      `m9:record-matches-plan-history` closes at M11.e. Of M10's four:
      `sky-field-round-trip` PASSES — `through the store: 2048 samples, lowest 0.00392157, highest 1`
      — and goes RED when `CloudShadowField::update`'s `publish()` is suppressed (`0 samples`);
      `fields-one-vegetation-potential` PASSES — `refused forwards 0, backwards 0` over 25
      declarations from 5 modules — and goes RED when foliage re-declares `vegetation-potential`
      (`refused forwards 1, backwards 1`); `fields-sampled-on-a-device` passes for the reasons 2.7
      records; and **`world-frame-budget` IS NOT CLOSED and its `known_gap` marker is still there**,
      re-measured on this host at **114.4 ms mean, 128.9 ms worst, 7.7x over at the worst frame**,
      with the same three bands the gap names — `terrain_shade_ms 69.0`, `sky_ms 24.5`,
      `water_ms 12.9`. Sections 2.3, 2.4 and 2.5 are why: `cy/field.slang` and its two consumers
      exist and compile, and **nothing in `samples/10-world`'s frame path dispatches them**.
      **The "not by weakening its check" clause is now evidence rather than a sentence**: the `run`
      strings of `sky-field-round-trip`, `fields-one-vegetation-potential` and `world-frame-budget`
      are BYTE-IDENTICAL to what `3d44e2f` (Close M10 — Worlds) shipped, so those two closures moved
      the code and not the check; `fields-sampled-on-a-device`'s `run` is the one that changed, and it
      changed in this repair, in the direction of a check that can fail
- [x] 10.7 **THE TWO CRITERIA THAT CARRY THE FOURTH GAP COULD NOT RUN AT ALL, AND THE PROVER FILED
      BOTH AS PROOFS — REPAIR 2.** `world-budget-headless` and `world-budget-on-a-device` ran
      `just run-sample 10-world … --cycle --budget-ms 16.7`. The sample is called `world`
      (`10-world` is the directory; `just/run.just` globs `samples/*/cy_sample_<name>` and its own
      comment says so), and neither `--cycle` nor `--budget-ms` was a flag `main.cpp` parsed — so
      both exited **2** at `no sample '10-world'`, on every machine, having measured nothing. Both
      were nevertheless recorded in `falsifiability.toml` as **`red against a built tree`**, the
      verdict a criterion earns by being watched failing for its subject's sake, with the exit-2
      line quoted as though it were the finding. `m11a:world-streams` was the third, with
      `--stream-report`, a flag that never existed either. **This is the ninth instance of this
      project's one defect and the second one committed by the mechanism built to end it** — the
      first was `absent-recipe`, which reads a `just` recipe's name and stops there.
      - **`falsify._inspect_samples`** refuses a `run-sample` name that no `samples/*/CMakeLists.txt`
        declares, in `CANNOT_GO_RED` beside `absent-recipe`. It found six criteria across four
        ledgers: the three above, `m11d:native-backend-runs-the-m0-sample` (`00-empty`, for a sample
        called `empty`), and `m11d:ship-sample-on-desktop` and `m11e:ship-sample-everywhere`, which
        name `11-ship` — a sample M11.e has not written, so those two are honest debt and are left
        as such, named in the audit rather than laundered. **Proven by the rule itself**: it fires
        on all six as written and on none of the three once repaired.
      - **`falsify._why_it_is_red`** records both ends of what a red criterion said. The old records
        carried `==> configure  profile=dev …` — the first line every build-backed criterion in the
        ladder prints — and nothing of the failure. `just`'s own epilogue is stepped over, because
        `error: Recipe … failed with exit code 1` is the same sentence for every failing recipe.
        The three re-recorded entries now read `the three largest bands: terrain_shade_ms 73.3 ms,
        sky_ms 27.4 ms, water_ms 14.2 ms` and `tiles_cooked: 49  tiles_resident: 49  evicted: 0
        stitched_vertices: 0` — the measurement, in the record.
      - **`--budget-ms` is in the program**, so the same arithmetic serves `m10:world-frame-budget`'s
        CSV and these two, and a person running the sample by hand gets the verdict the ledger gets.
        **It discriminates**: `--budget-ms 16.7` exits 1 (`OVER the budget`), `--budget-ms 1000`
        exits 0 (`INSIDE the budget at every frame of the cycle`) over the same take. `--cycle` was
        NOT restored — `main.cpp`'s header fixes that "a take is exactly one simulated day", so a
        flag asserting it has no false case; the take length is passed instead. A run that asked for
        a device and got none used to `return 0`; with `--budget-ms` it now exits 1, because a
        budget held by a run that drew nothing is the submit band missing from the total.
      - **`world-streams` reads a measurement instead of a flag.** The sample prints its residency
        off `TerrainStore::tile_count()` against what `build()` cooked — a difference, not a counter
        somebody has to remember to increment. Proven: evicting one level-0 tile moves the line from
        `evicted: 0` to `evicted: 1` with nothing in the reporter touched (`samples/10-world/world.cpp`
        md5-restored afterwards).
      - **THE FOURTH GAP IS STILL NOT CLOSED, and both criteria are still RED — now for the reason
        they name.** Measured on this host through the repaired command line: headless **121.1 ms
        mean, 128.8 ms worst, 7.7x over**; with a device drawing **137.2 ms mean, 156.0 ms worst,
        9.3x over**; bands `terrain_shade_ms 71.8`, `sky_ms 26.6`, `water_ms 13.8`. Sections 2.3,
        2.4 and 2.5 are still why. What changed is that a reader can now tell a criterion that ran
        and failed from one that never resolved its own command.
- [x] 10.8 **`m10:fields-sampled-on-a-device` WAS CLOSED FOR A REASON ITS DECLARATION DOES NOT NAME
      — REPAIR 2.** The declaration says closing it is "`cy/field.slang` written against the layout
      `gpu.h` already fixes, **bound through the GPU scene**, and **measured against a device**".
      After repair 1 the check ran the Slang front end and compared the checked-in SPIR-V — two real
      halves, and both of them PASS ON A MACHINE WITH NO GRAPHICS DEVICE. A gap whose subject is
      "an environment field is sampled on a device" cannot be discharged by a criterion that never
      asks for one. It now runs `render.environment_field` as its third half: the suite must be
      registered, must PASS, must not have skipped for want of a device, and must report its four
      comparisons with **0 unresolved** — and the criterion carries `requires = "gpu"`, with
      `fields-gpu-agreement` above it as the processor-side half every machine can judge, which is
      M8.c's rule. Green here over **17 152 comparisons through the bound buffer and through the
      bindless table alike**. **RED under mutation**: `commands.dispatch` removed from
      `tests/render/test_field_device.cpp` → `the device sampler does NOT agree with the processor
      over the same bytes` (11 of 7 986 assertions failed, 2 of 2 cases); restored and md5-verified.
      And `just roadmap-falsify prove m10 --only fields-sampled-on-a-device --build-dir … 
      --mutate-the-tree` now records it **`proven against a built tree`** — it was `not provable
      here` debt before — by renaming `cyFieldSampleScene` in `cy/field.slang`, rebuilding, watching
      it go red, restoring, and watching it come back green.
      **One defect in the new half was found by running it twice**: `ctest -N … | grep -q` under
      `set -o pipefail` gives ctest a SIGPIPE and the pipeline a 141, so the check reported "the
      suite is not registered" about a suite that is — a check failing for a reason that is not its
      subject, in the very repair that exists to remove those. It is a `case` over a captured string
      now, and three consecutive runs agree.
- [x] 10.9 **THE FOURTH GAP CANNOT BE CLOSED BY THE WORK ITS OWN DECLARATION NAMES, AND THAT IS NOW
      MEASURED RATHER THAN ARGUED — REPAIR 3.** Every run below is against the repository tree in
      `build/repair-3-0`, every mutation was restored and `md5sum -c` verified, and `git status` is
      clean of them afterwards.
      - **THE THREE THAT CLOSED ARE GREEN AND EACH WAS WATCHED GOING RED AGAIN HERE.**
        `sky-field-round-trip`: green at `through the store: 2048 samples, lowest 0.00392157,
        highest 1`; RED with `writer.value().publish()` in `src/rendering/sky/src/cloud_shadows.cpp`
        wrapped in `if (false)` → `through the store: 0 samples, lowest 1, highest 0`, EXIT=1.
        `fields-one-vegetation-potential`: green at `25 declaration(s) from 5 module(s); refused
        forwards 0, backwards 0`; RED with the M11.a diff to `src/foliage/src/system.cpp`
        reverse-applied, which reinstates `vegetation_potential_declaration()` → `26 declaration(s)
        … refused forwards 1, backwards 1`, EXIT=1. `fields-sampled-on-a-device`: green at 78 486
        SPIR-V words, both probes word-identical, 17 152 comparisons on the device; RED **once per
        half** — `/ 255.0` → `/ 254.0` in `cy/field.slang` gives `the checked-in SPIR-V is not what
        cy/field.slang compiles to today`, and `commands.dispatch` removed from
        `tests/render/test_field_device.cpp` gives `the device sampler does NOT agree with the
        processor over the same bytes`.
      - **AND NOTHING WAS WEAKENED, CHECKED RATHER THAN SAID.** Parsing both ledgers with `tomllib`:
        the `run` strings of `sky-field-round-trip`, `fields-one-vegetation-potential` and
        `world-frame-budget` are byte-identical to `3d44e2f`'s. `fields-sampled-on-a-device`'s is the
        one that differs, and repairs 1 and 2 record why — it gained the Slang front end, the SPIR-V
        equality and the device run.
      - **THE FOURTH IS STILL RED AND IS NOW WORSE THAN WHEN IT WAS DECLARED**: 115.7 ms mean,
        127.6 ms worst over the 64-frame take at 0x5EED on an idle host and 126.8/137.8 with this
        machine busy, against 122/106 in the declaration. Bands:
        `terrain_shade_ms` 68.7, `sky_ms` 25.4, `water_ms` 13.3, `ocean_ms` 5.6, `weather_ms` 1.7,
        `foliage_ms` 0.9.
      - **WHAT `water_ms` IS, MEASURED AND NOT INFERRED.** The band was split in place for one run:
        `drive_ocean_from_wind` is **0.00 ms** and `WaterSystem::tick` — `foam_.recentre()` then
        `foam_.advect()` — is **12.74 ms mean, 14.46 ms worst**. `water_ms` IS the foam field, which
        is the third of the three bands the gap names.
      - **THE CLOSING ACT, EXECUTED AND MADE SMALL, DOES NOT CLOSE IT.** Band one (`shade_terrain`)
        and band two (the `compose_sky` loop over the dome) removed from the frame, and the ocean
        patch build with them — what a shader doing them during the draw would leave behind:
        **21.6 ms worst, 18.7 ms mean, STILL RED at 1.3x over**, the remainder being the foam field
        at 14.1 ms. Remove the foam too and the same criterion reads **7.9 ms worst and passes**.
      - **SO THE CRITERION CANNOT GO GREEN FOR THE REASON ITS GAP NAMES, WHICH IS THE MIRROR OF THIS
        PROJECT'S SEVEN.** Those could not fail; this one cannot pass. Its `run` measures the sample
        under `--headless`, and `samples/10-world/main.cpp`'s `wants_pictures` opens a stage only
        when frames are asked for and `--headless` is not given, so **no shader of any kind executes
        inside the number being judged** — and the one band that would have to leave the processor
        for the take to fit is the one a headless run must do on it.
      - **WHAT CHANGED IN THE LEDGER, AND IT IS NOT A CHECK.** `m10:world-frame-budget` keeps its
        `describe` and its byte-identical `run`; its `known_gap` is restated to the measured numbers
        and the finding above, and `known_gap_closes` moves from `m11a` to **`m11c`** — the rung
        whose subject is the image, and which must write the three shaders AND settle what a headless
        budget claims once the visual work is on the device. `m11a:world-budget-headless` and
        `m11a:world-budget-on-a-device` stay RED at this rung, so the deadline moved and the failure
        did not. `m10:sky-field-round-trip`'s `describe` is rewritten: it asserted the sky's write
        path was broken, and `git diff 3d44e2f HEAD -- src/rendering/sky/` touches the README and the
        test and **not one line of `cloud_shadows.cpp`** — the store had the shadow all along, and
        what closed is the half the declaration's second sentence names, a case asserting against the
        producer's own statistics from twenty-five probes standing in full sun.
      - **WHAT THIS ROUND DID NOT DO.** 2.3, 2.4 and 2.5 are still unticked and honestly so: no
        shader is dispatched anywhere in `samples/10-world`'s frame path. And neither
        `m10:sky-field-round-trip` nor `m10:world-frame-budget` gained a `[criterion.falsifies]`,
        because no mutation the five verbs can express is faithful to either subject — suppressing a
        `publish()` that spans three lines, or moving three bands into shaders, is not a token rename
        or a line deletion — and a proof recorded on a mutation that misses the subject would be the
        defect wearing the mechanism's clothes. Both stay recorded debt, and the reds above were
        watched by hand instead.
