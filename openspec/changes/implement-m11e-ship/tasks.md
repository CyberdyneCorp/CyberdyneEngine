# Tasks: M11.e — Ship

Ordered. Section 0 is the spike and it runs first because **it decides whether sections 1 and 2
exist**: no toolchain file of any kind is in `cmake/`, no leg of `ci.yml` cross-compiles anything,
and `cmake/modules.cmake`'s own diagnostic says *"Supported: Linux, Windows, Darwin. Planned: iOS,
Android, visionOS, Web."* Scoping a mobile milestone against that without measuring it first is how a
rung arrives at its own gate with a deferral it could have declared on day one.

Section 6 is the sweep and it is last among the work because it cannot be scoped until the four rungs
above have closed. Sections 9 and 10 are the records and the gate, and **this gate is 1.0**.

**Section 5a is lettered rather than numbered, and deliberately.** It is scope moved here from M11.c
— the editor's material authoring front end, beside the `.cygraph` writing this rung was already
assigned — and `m11e.toml`'s criteria cite task numbers, so inserting a numbered section would
renumber the tasks those citations name. A letter keeps every existing number pointing where it
pointed.

## 0. The spike — can a hosted runner produce a mobile artefact at all?

- [ ] 0.1 **Measure, do not assume**, across the four axes `design.md` §1 separates, for **one empty
      project and nothing else**: toolchain acquisition inside a job's time budget; `cmake` configure
      for one mobile ABI with every enabled dependency resolving; `cy::core` compiling and linking for
      that ABI; and a published artefact a person could install or load. A single "mobile does not
      work" is not a finding — the axis that fails is the finding, because it names whether the
      obstacle is the runner, the build system, or one of the seventeen integrated dependencies
- [ ] 0.2 **State plainly what this host cannot answer**, through the ledger's own `requires`/`where`
      mechanism rather than as a sentence. This machine is Linux on x86-64 with one GPU vendor; there
      is no Apple toolchain, no Android device and no mobile runner in `ci.yml`. A criterion whose
      subject is a tiled GPU or an installed application reports **NOT EVALUATED** with a reason —
      never a pass. M9's `lockstep-cross-platform` is the shape and M10 used it twice
- [ ] 0.3 Commit the spike to `~/cyberdyne-spikes/m11e-mobile-spike/`, outside the repository as the
      seven before it were, and record its answer in `design.md` §1 so the rows that depend on it can
      read it without re-deriving it
- [ ] 0.4 **Fire the contingency, or record that it did not.** If toolchain acquisition or configure
      fails, mobile becomes a **recorded deferral with a re-entry point**, this rung is the
      distribution and record rung, and `build-system-and-platforms` and `rendering-forward-clustered`
      are re-scoped through changes against their own specifications — `delivery-roadmap`'s *"a spike
      may resize a milestone as well as redirect it"* is the requirement that permits it, and the
      change lands **before** the rest of the rung proceeds

## 1. Mobile targets and cross-compilation — `build-system-and-platforms` → C

Contingent on section 0. Every task below assumes the spike cleared its axis; where it did not, the
task becomes a deferral with its three parts (§6.1) rather than a smaller version of itself.

- [ ] 1.1 **A CMake toolchain file per target, documented, and there are zero today.** `cmake/` holds
      seven files and none of them is a toolchain. The requirement is *"cross-compilation through
      CMake toolchain files, with a documented toolchain per target"*, and *"SHALL not require the
      host and target to match"* — so the check is a cross-compile from the x86-64 host, not a native
      build on a runner that happens to match
- [ ] 1.2 **One mobile platform ported**, implementing the porting surface the requirement enumerates:
      `Platform`, `DisplayServer`, an input backend, an audio backend, a graphics surface provider per
      enabled RHI backend, and packaging and deployment support — **under `platform/<name>/`, with no
      `#ifdef` in a shared file**, and requiring no change in `src/core/`, `src/ecs/`, `src/servers/`
      or `src/scene/`. M11.d proves that property against a second *desktop*; this is the first time
      it is asked of something that is not a desktop
- [ ] 1.3 **`cmake/modules.cmake`'s "Planned" list gets shorter by at least one entry, or it does
      not and the diagnostic says which.** The list at line 317 is the build system's own statement of
      what it supports; a module manifest naming `android` builds nothing today and the diagnostic is
      where that is true or false
- [ ] 1.4 **The seventeen dependencies, one mobile ABI.** SDL3 is how this engine opens a window on
      every target it has. Which of the seventeen has no mobile build is section 0.1's second axis and
      this is where the answer is paid for — vendored, replaced behind its engine-owned interface, or
      feature-gated off for that target with the capability it removes named
- [ ] 1.5 A cross-compilation leg in `ci.yml` that builds for the mobile target on every pull request,
      so a change that breaks the port fails before merge rather than at the next release

## 2. The mobile pipeline — `rendering-forward-clustered` → C

**M11.d owns MSAA and multi-view; this rung owns the mobile differences and the row's tier.** Two
rungs, one row — `design.md` §2 — and the tier is not recordable until both halves are in.

- [ ] 2.1 The five differences the requirement names, each as a **declared property a check can read**
      rather than a documented intention: the depth prepass omitted by default, per-object light lists
      bounded per object rather than a cluster grid, tonemapping as a **subpass** so HDR colour never
      leaves tile memory, screen-space reflections and GI and subsurface scattering omitted, and
      memoryless attachments preferred for depth and MSAA targets
- [ ] 2.2 **Assert them on the null backend.** Every one of the five is a property of the render
      graph's own declarations — an attachment's memoryless flag, a pass's subpass membership, a
      culling mode, a pass set — and the null backend records every command and hashes the stream,
      which is the instrument M3's XR prerequisite checks already use for exactly this kind of
      structural question. This is what makes the row checkable without a phone
- [ ] 2.3 **And say what that does not prove.** The requirement's own scenario names the hardware:
      *"WHEN the mobile pipeline runs on a tiled GPU THEN the HDR colour attachment SHALL be declared
      memoryless and resolved to the swap chain within the same render pass."* On this host and on
      every leg of `ci.yml` that scenario reports **NOT EVALUATED** with its reason. `design.md` §4
      predicts this row is the demotion; if the gate judges that a row cannot be Complete while the
      scenario naming its hardware has never run, **the row is demoted to Working with the mobile half
      deferred and its re-entry point recorded**, which is what a demotion is for
- [ ] 2.4 **Compute workloads use the mobile backend too.** Package backend-native compute shaders
      for GPU skinning and GPU VFX, run both through Metal on a physical Apple GPU, and compare their
      outputs against the existing CPU reference paths. A forward-only mobile frame does not prove
      that compute-driven geometry or effects can ship on the platform.

## 3. The dependency set and attribution — `thirdparty-dependencies` → C

**This is the row this rung cannot finish by working harder** — `design.md` §2. Six to ten adoptions
are performed by M11.a, M11.b and M11.c against requirements on this row, and this rung records the
tier.

- [ ] 3.1 **Turn *"about half the intended set is not integrated"* into a list before anything is
      scoped.** The intended-set table names roughly forty-two libraries; `deps/manifest.toml` carries
      **seventeen** `[[dependency]]` entries, one of which (`eigen`) is a transitive dependency of
      ONNX Runtime rather than a table entry. Sixteen of the table's entries are declared. The
      difference is the scope, and it is longer than the phrase suggests
- [ ] 3.2 **Each un-integrated entry ends in one of three states and none of them is silence**:
      integrated; removed from the intent through a change against this specification with the reason
      recorded; or deferred with a re-entry point. *"Where a table entry is marked to evaluate, the
      requirement is the capability, not the library"* is already the specification's own escape for
      **ACL** and the **QUIC or reliable-UDP** row, and it is the honest route for others — but it is
      a change, not a reading
- [ ] 3.3 **A manifest entry is not an integration, and `steam_audio` is the proof.** It has carried a
      complete entry since M8.c — version, tag, commit, licence, interface, scope, justification — and
      `-D CY_AUDIO_STEAM_AUDIO=ON` does not configure, because upstream's CMake calls
      `find_package(PFFFT)`, `find_package(IPP)` and `find_package(FFTS)` and the manifest provides
      none of them. **The criterion for this row counts what fetches, builds and links — not what is
      declared.** Closing it is M11.a's task; counting it honestly is this one's
- [ ] 3.4 **The runtime attribution API, which does not exist.** The requirement: *"The engine SHALL
      ship a complete attribution document generated from the manifest, and SHALL expose it at runtime
      so games can display required notices without assembling them manually."* `THIRD_PARTY.md` is
      generated and the runtime half is absent — nothing in `src/` outside `cy::core::memory`'s own
      unrelated `MemoryAttribution` answers to the word
- [ ] 3.5 **It reports exactly what is linked into *that* build, which is the half a generated
      document cannot do.** A build with `CY_AUDIO_STEAM_AUDIO` off SHALL NOT list Steam Audio; a
      build with `CY_ML_ONNXRUNTIME` off SHALL NOT list ONNX Runtime **or Eigen**, which is in the
      tree only because ONNX Runtime reaches it. A test that runs the query with the option on and off
      and requires the two answers to differ is the check; one that only runs it on is not
- [ ] 3.6 **The runtime footprint requirement, checked rather than asserted**: Slang, SPIRV-Cross,
      texture encoders, mesh processing, UV unwrapping, the glTF and FBX parsers and Recast generation
      are **tool-time only**, and a shipped game containing any of them is a failure the attribution
      query can now see

## 4. Distribution — versioned release recipes and artefacts

- [x] 4.1 **`release-version`, `release-changelog`, `release-artefacts` and `release-publish` stop
      refusing.** All four are `_not-implemented` stubs naming *"M12 — build-and-packaging"*, and
      **there is no M12**: `record.MILESTONES` is fifteen entries ending at `m11`. M11's exit criteria
      include *"Version, changelog and artefacts are produced by the release recipes"*, so a stub is
      a failing exit criterion and not a deferral
- [x] 4.2 **Distribution artefacts, as the requirement enumerates them**: the editor application,
      runtime libraries for embedding, the C ABI headers and ABI description, the `CyberdyneKit` Swift
      package, runtime templates per platform and configuration, and the tools — build service,
      cooker, packager, shader compiler. **Names encode platform, architecture, configuration and
      version**, and every produced build carries `build-and-packaging`'s provenance record
- [ ] 4.3 **One execution path.** *"WHEN the same build is produced from the editor and from the
      command line THEN both SHALL drive the same service, graph, and cache, and produce identical
      artefacts"* — which is a byte comparison between two artefacts, and therefore a criterion that
      can fail
- [ ] 4.4 **Downloadable content and distributed execution, if M11.d has not carried them.**
      `build-and-packaging` is M11.d's row and its proposal predicts it may demote exactly this half;
      `docs/roadmap/risks.md` already lists distributed build execution as deferred to *"M11 or
      later"* with the derivation graph as the seam held open. **If it arrives, it arrives with the
      reason that demoted it** — `design.md` §2 — and it ends Complete or deferred with three parts,
      never as a row that quietly changed owner
- [ ] 4.5 **A check that a refusing recipe names a rung that exists and has not closed.** Three
      classes are live on this tree: the four release stubs name **M12**, which is not on the ladder;
      `build-shaders` names **M3**, which closed eight rungs ago while `slangc` is integrated and 26
      `.slang` modules are in the tree; and `maintenance-clean` names task **"2.1.5"**, which is not a
      milestone at all. The same defect was found and corrected **by hand, in one file** at
      `just/content.just` line 149 — which is why this is a check rather than a fourth edit.
      **`build-shaders` and `maintenance-clean` belong to `developer-workflow-and-just`, M11.d's row**;
      correcting a stale label is a one-line edit and is not this rung absorbing that capability

## 5. The full continuous-integration matrix

- [ ] 5.1 **M11.a builds the first job that compares two legs; this generalises it and does not
      rebuild it.** `ci.yml` runs `linux-arm64`, `macos-arm64` and `windows-arm64` beside their
      x86-64 siblings in two matrices and **nothing compares or combines them**. Two jobs that each
      define what a leg is would be two definitions that drift
- [ ] 5.2 The requirement's own list, on every pull request: build all supported platforms in
      `Development` and `Shipping`, run the test suites, run static analysis and formatting checks,
      verify the ABI baseline, verify generated code is current, and **produce a licence report** —
      which section 3.4's attribution query is now able to generate rather than assemble
- [ ] 5.3 The nightly half: sanitiser builds, longer suites, and **performance benchmarks with
      regression detection reported with the commit range**, so a regression is bisectable rather than
      merely noticed
- [ ] 5.4 The mobile leg from 1.5 joins the matrix, or the matrix records its absence as part of the
      mobile deferral rather than being quietly one leg short

## 5a. Inherited from M11.c — the editor's material authoring front end, beside the `.cygraph` writing it depends on

**Lettered rather than numbered so that `m11e.toml`'s `source` fields, which cite task numbers, keep
citing the same tasks.**

**WHY THIS IS HERE AND NOT IN M11.c.** M11.c's spike ran its rung's whole authoring path and junction
1 — AUTHOR — came back **REFUSED**: `SpecialisedEditors::open(Domain::Materials)` returned *"this
build declares no authoring vocabulary for materials"*. Behind that refusal was not a fix but three
pieces in two languages — a node-type vocabulary in Rust, a material document and its on-disk form,
and a `lower_material` in `src/graph/` — which M11.c's proposal costed as one line of one task.
**M11.c built two of the three and the third was already this rung's**:
`editor/crates/cy-editor-interface/src/specialised/graph.rs` assigned **writing `.cygraph` from Rust
to M11.e** before M11.c started, and gave the reason — *"a second writer of a canonical format is a
second format the day the two disagree about a float"*. M11.c design.md §1.3c records the move.

**WHAT ARRIVES ALREADY BUILT, so this section adds a front end rather than a vocabulary**:
`src/graph/material/`'s 25 node types (one per `GraphOp` plus `material.output`, asserted equal to
`"material." + graph_op_name(op)` by `unit.graph_material`); `Domain::Materials => MATERIAL_NODES`
and `specialised/material.rs`'s pins, without which `GraphCanvas::connect` refuses every wire; and an
**interchange** the editor writes and `cy_material author` canonicalises through the engine's own
`write_graph`, so there is exactly one writer of `.cygraph` today and it is the engine's.

**AND WHAT ARRIVES AS A CAPTION THIS RUNG CAN MAKE OBSOLETE.** M11.c's beauty shot was authored
through the tooling that exists rather than through a material graph editor: its three materials are
placed and wired on the editor's authoring MODEL by `cy-author-material`, a binary in the editor's
own workspace — not by a person at a window and not over the control socket.
`m11c:the-shot-does-not-overclaim-the-editor` asks `cyberdyne-editor --list-commands` for the
registry and **goes RED the day a `material.*` command appears in it**. So task 5a.1 below breaks a
green criterion on another rung's ledger by succeeding, and the caption in
`docs/design/beauty-shot.md` is owed an update in the same change. That is the criterion working, not
failing, and it is named here so that whoever lands 5a.1 is not surprised by it.

- [ ] 5a.1 **`material.*` commands in `cy_editor_services`' registry**, so a material can be authored
      over the control socket the way `samples/08a-authoring` authors a scene. There are none today —
      measured, not assumed: `cyberdyne-editor --list-commands` prints the registry and no entry
      begins `material.`. Until they exist a person at a window cannot place a material node, and the
      only thing that can is a program linking `cy-editor-interface` directly
      - [ ] **The criterion is the round trip and not the listing.** A registry entry that no canvas
        honours is the same shape of defect as a catalogue with no pins. What has to go red is a
        command sequence that places and wires the shot's own material and produces the committed
        `.cygraph` — the file `m11c:materials-are-textures` already regenerates and compares
        byte for byte, so the comparison exists and only the driver is new
      - [ ] **Update `docs/design/beauty-shot.md` and M11.c's `the-shot-does-not-overclaim-the-editor`
        in the same change**, because that criterion is written to fail the moment this lands. Its
        `describe` says so in its own words. **Do not weaken it to keep it green** — the honest edit
        is the caption, which stops being an admission and becomes a record of how the shot was
        authored on the day it was authored
- [ ] 5a.2 **The material graph front end, and the stage comparison that has been waiting for one.**
      M11.c task 1.3 asked that *every lowering stage `cy_material compile` prints be reachable
      through the editor's front end, and the two agree on the same graph*. The engine half is done —
      `stages.h`'s five stages, `dump_graph`/`dump_module`, `cy_material compile --stages`,
      `attach_backend_stage` — and the comparison there runs the command line against a second caller
      of the same list, which is a stand-in and is labelled one. **The front end is the missing
      operand.** `material-compiler`'s *"Node previews use the real compiler"* and `shader-system`'s
      *"Visual material editor"* are the same absence read from two sides
      - [ ] **The check compares two answers, not one answer against a fixture.** The stage list and
        each stage's output that the front end obtains, against `cy_material compile --stages` for
        one committed graph, red when they diverge. A fixture of expected stage names would agree
        with a front end that had stopped asking the compiler
      - [ ] **`src/rendering/material/src/preview.cpp`'s refusal is the negative control and it
        already exists.** `m11c:material-node-previews` mutates it: delete the one-statement guard
        and a preview is attempted with no compiler behind it. A front end that draws a thumbnail
        when the compiler is gone is the defect both rows' requirements name, and the control for it
        is written and proven red
- [ ] 5a.3 **Writing `.cygraph` from Rust — the assignment this section is named after — is a
      decision to make, not a feature to add.** `specialised/graph.rs` forbids a second writer of the
      canonical format and gives the reason. The interchange M11.c built is one answer and it may be
      the right one permanently: the editor writes a source, the engine owns what it becomes, the way
      M8.a chose for `.cyprim`. **If that is the answer, say so and retire the comment's "M11.e"
      rather than leaving a rung pointer that outlived its rung**; if it is not, the round trip needs
      a digest comparison against the engine's writer before any Rust-written file is committed
- [ ] 5a.4 **And the VFX half is the same shape and is NOT silently included here.**
      `Domain::VfxGraph::node_types()` is still the empty slice and
      `SpecialisedEditors::open(Domain::VfxGraph)` refuses exactly as `Domain::Materials` did. M11.c
      fixed one of that pair and demoted `vfx-system` to Working for the other, so the row arrives at
      section 6's sweep **with the reason that demoted it** and is settled there — Complete or
      deferred with a re-entry point — rather than absorbed into this section. The engine-side
      vocabulary a VFX catalogue would need does not exist either, which is what makes it a row for
      the sweep and not a front end for 5a.2

## 6. The sweep — every remaining row Complete, or deferred with a re-entry point

**This rung is the only one on the ladder permitted to record a deferral**, and that permission is
what makes 6.1 mandatory rather than advisory.

- [ ] 6.1 **Every deferral carries three things, and one with fewer is refused**: what is unmet, read
      requirement by requirement against the tree at the gate rather than against the scope statement
      written before the work; **why** — a measurement or a decision, named, because "ran out of
      milestone" is a schedule and not a reason; and **the condition that brings it back**, a state of
      the world rather than a date. `docs/roadmap/risks.md` already carries eleven deferrals in
      exactly this form and this rung extends that table rather than starting a second list
- [ ] 6.2 **Each row demoted by a rung above arrives with the reason that demoted it.** Six are named
      in advance — `save-and-persistence` and `audio` (M11.a), `ml-inference` and `swift-scripting`
      (M11.b), `rendering-culling-and-lod` (M11.c), `build-and-packaging` (M11.d) — and an unknown
      number are not. **A row that arrives with no reason is a row that has to be finished, not
      recorded**
- [ ] 6.3 **A second demotion of one row is a finding about the plan, not about the schedule.** M10's
      `design.md` §4 wrote that rule down in advance and `save-and-persistence` is what it caught. Any
      row reaching this rung having been demoted twice is re-scoped through a change against its own
      specification before it is recorded either way
- [ ] 6.4 **The 1.0 record is a criterion, not a document.** `m9:record-matches-plan-history` is the
      lesson: four rows' Working tier is claimed by a matrix column and evaluated by **nothing**,
      because the only `expect_tiers` entry naming any of them expects `seed` and `_check_tiers`
      treats an exit tier as a **floor**. So the check reads the record and the deferral list and
      **fails naming any row that is neither Complete nor deferred with all three parts** — it goes
      red today, on 66 rows, which is what makes it a criterion
- [ ] 6.5 **`delivery-roadmap` → Complete**, which is the row that cannot close before the sweep does
      and therefore closes last by construction

## 7. `xr-support` — the deferral restated, and the one seam that is half open

- [ ] 7.1 **The three prerequisite checks still pass on the day 1.0 is recorded.** They are
      `tests/render/test_xr_prerequisites.cpp`, running on the null backend since M3 through
      `m3:xr-prerequisites`: two views are **one** submission of the geometry, the frame's inputs are
      **arguments** rather than a clock, and the view reaches submission **through memory** rather
      than through a re-recorded command
- [ ] 7.2 **And one of the three is half open, which the record already says and no check reports.**
      `m3.toml`'s own note: *"THE XR FRAME-TIMING SEAM IS HALF OPEN ... The half that is NOT open is
      `cy::Runtime::tick()`, which takes no predicted display time, so a host cannot yet tell the
      engine when a frame will be displayed."* At 1.0 that is resolved deliberately — either `tick()`
      accepts a predicted display time, or the partial closure is recorded as a decision with its cost
      through a change against `xr-support`. **A seam check reporting green over a half-open seam is
      worse than no check**, and it is the only prerequisite this rung has to do anything about
- [ ] 7.3 **`xr-support` stays deferred, with its re-entry recorded as it already is** — *"after
      1.0"*, prerequisites held open. This is a decision restated, not a row skipped, and it is the
      one row of seventy-six that reaches 1.0 without a Complete cell

## 8. The artefact — `samples/11-ship`, and the 1.0 record

- [ ] 8.1 **`samples/11-ship` on every supported target from a single recipe.** `samples/` holds
      fifteen entries and none of them is a packaged project. M11.d builds the desktop half; this rung
      adds the mobile target if section 0 cleared it, and records its absence with a re-entry point if
      it did not
- [ ] 8.2 **Built, cooked, packaged and launched** — four verbs, and *packaged* and *launched* are the
      two this project has never done. A recipe that produces a package nobody installed is a build
      claim wearing a ship claim's name
- [ ] 8.3 **Capture it.** The rule since M8.c holds: anything with a visible result gets an image
      under `docs/design/images/`, a diagram is labelled one, and **a picture of the application
      running on the target platform is the only evidence that distinguishes a cross-compile from a
      port**. Where no device exists to photograph, that is recorded rather than substituted
- [ ] 8.4 **The 1.0 record itself**: seventy-six rows, each Complete or deferred with a re-entry
      point, the status record, the capability matrix and the fifteen-plus ledgers agreeing, and **the
      statement of what 1.0 is and what it is not** — which capabilities are complete, which are
      deferred, and what a person adopting this engine should and should not expect of it

## 9. Records and gates

- [ ] 9.1 Write `tools/roadmap/milestones/m11e.toml`; declare `milestone-m11e` in `gates.toml` and
      raise `selftest.MINIMUM_CRITERIA`. **The rung `m11e` itself is placed on `record.MILESTONES` by
      M11.a's split change**, not by this file — `delivery-roadmap`'s *"a milestone identifier that is
      not on the ladder SHALL be a configuration error reported by `roadmap-test`"* is what would
      catch it if it were not
- [ ] 9.2 **There is no `m12-open` criterion, and the absence is declared rather than left.** Every
      rung since M8.c has closed by opening the next one — `m8c:m9-open`, `m9:m10-open`,
      `m10:m11-open`, each a double-star glob over `openspec/changes/`. **This rung has no next rung**,
      and what replaces that criterion is 6.4's 1.0-record check plus an explicit note in the ledger
      saying why no `*-open` criterion exists here, so a reader does not read the absence as an
      omission
- [ ] 9.3 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`, and run
      the plan-consistency checks over them. **`ROADMAP.md`'s M11 row and exit criteria are the scope
      statement this ladder implements**, and its eight exit criteria are spread across five rungs; at
      this gate every one of them is answered or deferred
- [ ] 9.4 Move `ci.yml`'s milestone job to `m11e` in the same commit that flips the gate green. The
      job runs on every push to `main`, so the commit must not land before the gate is green — M10
      task 8.4 measured both states rather than assuming them and this repeats that
- [ ] 9.5 **The deferral table in `docs/roadmap/risks.md` is extended, not replaced**, with every row
      the sweep defers, in the column form it already uses: what is deferred, where it is recorded,
      the seams held open, the check that proves they are, and when it is reconsidered

## 10. The gate

- [ ] 10.1 **Full audit, per `delivery-roadmap`, not the reduced form.** The reduction is *permitted*
      from M9 and its premise is that something other than the audit is checking the claims; **this is
      1.0**, and the claims are every row of the record at once. Run it in full and record what the
      reduced form would have missed
- [ ] 10.2 Clean build of every profile from empty; `test-all` in each; every gate by hand; the M11.e
      ledger run once
- [ ] 10.3 **Every criterion executes something and can fail** — break what it checks and prove it
      goes red. Every gate on this ladder has found a claim exceeding what was checked, and the
      large majority of those findings were in the **checking** rather than in the engine
- [ ] 10.4 **Adversarial pass on this rung's own invariants**, which is the part no accumulated check
      inherits: delete a deferral's re-entry point and confirm 6.4 goes red; record a row as Complete
      that no criterion evaluates and confirm it is refused; point a refusing recipe at a milestone
      that does not exist and confirm 4.5 names it; disable an optional dependency and confirm the
      attribution query stops listing it; break the mobile pipeline's memoryless declaration and
      confirm 2.2 fails on the null backend
- [ ] 10.5 **Records verified against what the code supports, not against what the plan claimed** —
      including this rung's own Complete cells, through `m9:record-matches-plan`, which is not
      milestone-specific in shape and is the check M9's gate added for exactly this
- [ ] 10.6 **The honest 1.0 statement is written at the gate, after the audit, not before it.** A 1.0
      record assembled from the plan rather than from the audited tree is the one unforgivable
      outcome, and every gate on this ladder has found at least one claim that exceeded its evidence
