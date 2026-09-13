# Tasks: M10 — Worlds

Ordered. Section 0 is the spike and it runs first because region invalidation decides the shape of
every producer that writes into the substrate, and section 1 is the substrate because seven of this
milestone's eight rows write into it.

## 0. The spike — region invalidation in PCG

- [x] 0.1 Measure, do not assume: a dependency-driven partial regeneration over a graph with known
      edges, against a full-world regeneration of the same seed, compared for identity of output and
      of generated identity. A partial regeneration that does not reproduce the full one is the
      finding, and it is a finding about the graph's dependency model rather than about the seed
- [x] 0.2 State plainly what the spike cannot answer on this host, through the ledger's own
      `requires`/`where` mechanism rather than as a sentence
- [x] 0.3 Commit the spike and record its answer in `design.md`, the way M8.b's IR spike and M9's
      determinism spike were committed and consumed without re-deriving them

## 1. The substrate — one producer per field

- [x] 1.1 Field declaration and the sparse tiled store; CPU and GPU access to the same field
- [x] 1.2 **One producer per field, refused at registration** — a second producer for the same field
      fails, naming both producers. The exit criterion says "a second producer registration fails",
      so the refusal is the criterion and it must be shown to fail with the check removed
- [x] 1.3 Residency levels and streaming, against `world-partition-and-streaming`'s existing cells
- [x] 1.4 Determinism of gameplay-visible fields, and the firewall for presentation-only ones —
      `simulation-and-determinism`'s classification, not a second mechanism beside it

## 2. Terrain, water, foliage

- [x] 2.1 Tiled hierarchical storage; terrain as a geometry source; material layers and frequency
      separation
- [x] 2.2 Deformation classes and deltas through the persistence overlay; collision and the
      navigation contribution; HLOD and the modifier stack
- [x] 2.3 Water bodies, the displacement contract, spectral ocean, rivers, shoreline, surface and
      underwater shading, foam, caustics, queries, buoyancy
- [x] 2.4 Foliage instances that are not entities, clusters, promotion, deterministic procedural
      placement, GPU grass, wind response, the interaction field, the budget

## 3. Weather, wind, atmosphere

- [x] 3.1 Climate and weather cells, environment sampling, the wind field
- [x] 3.2 Precipitation, wetness and snow, storms, presets and transitions, ecosystem state
- [x] 3.3 The physical atmosphere and its tables, aerial perspective, celestial bodies, volumetric
      clouds and their shadows, planetary scale

## 4. Procedural content generation

- [x] 4.1 Typed datasets, compiled graphs, execution domains, deterministic derivation
- [x] 4.2 **Stable generated identity**, and a hand-placed override that survives regeneration of its
      region — both are exit criteria and both are tests before they are prose
- [x] 4.3 Regions and spatial invalidation from section 0's answer; caching; output adapters;
      provenance; persistence of generated content

## 5. `vfx-system` → Working

> Built, device-tested and confirmed by M10's gate, which broke each claim and watched it go red.
> `cy::vfx-gpu` in `src/vfx/gpu/` keeps `cy::vfx` device-free, so `integration.vfx` stays headless.
> Boxes ticked after the fact: agents do not commit, so nothing ticked them at the time.

- [x] 5.1 Particle state resident in GPU buffers; indirect dispatch driven by GPU-maintained counts
- [x] 5.2 Async compute where the device exposes a queue; the GPU sort behind `BudgetLevers::sorted`
- [x] 5.3 The renderer kinds beyond `Sprite` and `Mesh`

## 6. The three rows M9 demoted, and the gaps it declared

- [x] 6.1 **`diagnostics-profiling-and-crash` → Complete**: adopt `CY_BREADCRUMB` at the five
      boundaries the specification names, and repair `m9:crash-artefact-paths` — a module table
      captured at install time through `dl_iterate_phdr`, with the fault path writing addresses plus
      a basename. Both criteria are declared gaps that CLOSE here, and a declared gap that starts
      passing fails the ledger, so closing them is checked rather than claimed.
      **Done in code**: the five boundaries are `Simulation::step`, `Simulation::run_stage`,
      `AssetSystemImpl::publish`, `CellActivation::publish` and `SaveService::perform_write`, each
      asserted by its own module's suite through `cy/test/breadcrumbs.h`; the module table is
      `crash_handler_posix.cpp` and `diagnostics.crash` greps a produced artefact for a build path.
      Both declarations are deleted from `m9.toml`. **The row's tier cell is the closing gate's**
- [ ] 6.2 **`save-and-persistence` → Complete**: confidentiality over the existing container — a
      vetted AEAD, which is a dependency decision through `thirdparty-dependencies` rather than a
      coding task — and conflict resolution.
      **NOT DONE, AND THE ROW IS DEMOTED A SECOND TIME.**
      **Half of it landed**: conflict resolution is `src/save/include/cy/save/conflict.h`, a decision
      over the logical metadata the specification names — generation, campaign identity, simulation
      point, progress marker, content version — taken on a type that HAS NO TIMESTAMP FIELD IN IT, so
      it cannot be decided on a file's modification time even by accident. `Manifest::progress`
      carries the progress marker and is written only when non-zero, so a save written before this
      field existed decodes rather than failing on a missing required field. Eight unit cases, plus
      one over two real save directories in which the copy that must LOSE is the newest file on disk.
      **The other half did not**: confidentiality needs a vetted AEAD, `thirdparty-dependencies`
      names mbedTLS as this engine's cryptography library, and adopting a dependency "SHALL go
      through the OpenSpec change flow recording the evaluation against these criteria" — a change of
      its own, not one to open inside a closing gate, and M10 added no dependency at all.
      **AND THE AUDIT FOUND MORE THAN THE TWO BLOCKERS M9 NAMED.** Read requirement by requirement
      against this tree, `save-and-persistence`'s twenty requirements come out **nine satisfied,
      three unmet and eight partial**. The three unmet are integrity and confidentiality (the AEAD);
      save diagnostics and inspection (there is no inspector, nothing answers "why is this field in
      the save", and there is no semantic diff); and forbidden save patterns, of whose ten "each
      SHALL be checkable" and none is checked. Among the partial is the **large-world save benchmark
      the requirement says the engine "SHALL maintain"**, which `benchmarks/` does not contain. The
      table, with the evidence for each row, is in `src/save/README.md`.
      design.md §4 wrote down in advance what a second demotion of one row means — mis-scoped rather
      than late. A Complete cell for this capability costs eleven pieces of work rather than the two
      M9 named, and **that is the finding for M11's proposal**
- [x] 6.3 **`gameplay-framework` → Complete**: benchmark the specification's performance table
      rather than assert it, closing `m9:gameplay-benchmarks`.
      **Done in code**: `benchmarks/gameplay/` with five committed thresholds covering the five rows
      of the table a measurement can speak to; `bench_gameplay.cpp` names the two it cannot and why,
      and `benchmarks/README.md` carries the frame-time fraction the table's last sentence asks for
      and the hybrid-CPU reason its tolerances are wide. The declaration is deleted from `m9.toml`.
      **The row's tier cell is the closing gate's**
- [x] 6.4 **`tests/determinism/` stops being empty.** The suite `testing-and-quality` gives a
      location and a 10 s budget: golden replays with committed hashes replayed in CI, replay and
      save fuzzing, and the transactional save tests. `cy_add_test`'s taxonomy gains the kind its own
      error message already promises
- [x] 6.5 **`m9:record-matches-plan-history`**: audit the four closed milestones whose matrix columns
      claim nineteen cells the status record does not support, thirteen of them Complete, and move
      or claim each one. An OpenSpec change against `delivery-roadmap`, because moving a capability
      between milestones is one by that specification's own rule.
      **Done**: all nineteen read against the tree M10 closes on rather than against the gate that
      parked them — two had moved since, `live-editing` gaining the other half of its bridge at M7
      and `editor-viewport-and-gizmos` engine-side picking at M8.a. **FIFTEEN CELLS MOVED**
      (`editor-architecture` and `live-editing` W→S at M5; `project-and-plugins` and eleven M8.b
      Completes to M11; `developer-workflow-and-just`'s W from M5 to M6) and **FOUR ROWS CLAIMED** —
      `testing-and-quality` Working at M3, `build-system-and-platforms` at M4,
      `developer-workflow-and-just` at M6, `thirdparty-dependencies` at M8.b — because their columns
      were right and their record had never been written. Per-cell evidence in
      `docs/roadmap/capability-matrix.md#m10s-record-audit-nineteen-cells-over-four-closed-milestones`;
      the change is `openspec/changes/audit-closed-milestone-columns/`. The declaration is deleted
      from `m9.toml` — a declared gap that passes fails the ledger — and the `tier_rank()` crash the
      same check carries for an unstarted capability is guarded there too. **M11's load goes 48 → 61**
- [x] 6.6 **`m9:reliable-channel-stalls-under-loss`** — the second M9 gap naming M10 as its closing
      rung, and the one no task named. An omission corrected rather than new scope.
      **Done, and it was TWO defects rather than the one M9 guessed at.** M9 read "`abandoned()` has
      no caller" as the cause; the cause was the replay window. `already_received()` calls anything
      more than 32 sequences behind the newest arrival a replay — right for an unreliable datagram,
      wrong for one retransmitted across a 7.5 s horizon, which is about 470 sequences at 60 Hz — so
      the third retransmit attempt onwards was refused and `next_ordered_` froze for the rest of the
      session. `ReliabilityChannel::accept_ordered()` now answers from the delivery frontier plus the
      reorder buffer, which is EXACT rather than conservative, and acknowledges by NAME what
      `ack_bits` cannot reach; nothing on the wire changed. That took the session from **0 of 4
      converged at 25 % loss to 3 of 4**. The fourth was the second defect: the default
      `RetransmitPolicy` spreads ten attempts over longer than the session itself, and no backend
      exposed `ReliableEndpoint::set_policy()`. Both transports carry `set_retransmit_policy()` now
      and `samples/09-multiplayer` caps its backoff at two round trips — **4 of 4 at nine seeds in
      nine**. `abandoned()` has its reader at last, `ConnectionStats::reliable_abandoned`, printed
      as `session_abandoned_links`: 0 of 8 at 25 %, 4 of 8 at 50 %. Two regression cases, each
      verified red against a faithful restoration of what it checks. The declaration is deleted from
      `m9.toml` — a declared gap that passes fails the ledger

## 7. The artefact — `samples/10-world`

- [x] 7.1 Procedurally populated terrain with rivers and an ocean; foliage responding to a wind field
      driven by weather; wetness and snow accumulating; a full day/night cycle with volumetric clouds.
      **Done in code**: `samples/10-world`, the first target in the tree that names all seven of this
      milestone's modules. **NO RIVERS**: `cy::water`'s `RiverNetwork` is built and tested and this
      world has an ocean and a fjord coastline and no river spline in it — see the sample's README
- [x] 7.2 Streamed and persistent: the world saves, resumes and patches, against M6's own artefact.
      **PARTLY**: a crater goes through `terrain::TerrainDeltaStore` into `world::PersistenceOverlay`
      as a subsystem blob, the delta store is cleared and verified empty, and the restore reproduces
      289 of 289 probes bit for bit — and the program EXITS NON-ZERO if it does not, so a regression
      fails `just capture-world` rather than printing a different number into a log. What is NOT
      done is the field half — weather's `wetness` and `snow-depth` are declared `persistent` and
      nothing encodes them — and there is NO STREAMING in
      this artefact at all: the whole world is resident. Both are recorded in the sample's README
- [x] 7.3 The environment demo holds its frame budget across a full day/night cycle, **measured as a
      curve across the cycle** rather than asserted at one time of day.
      **MEASURED, AND IT DOES NOT HOLD ONE.** `--budget <csv>` writes every frame's cost per
      producer and `docs/design/images/m10-world-budget.png` plots it: about 122 ms mean and about
      145 ms worst, NEARLY flat across the cycle — the only band that knows the time of day is the
      cloud march, 25.5 ms with the sun up against 21.7 ms with it down, in every capture. Six
      captures of the same seed spread 121.6-123.9 ms mean and 140.9-147.5 ms worst, so the tenths
      are this host's load rather than this world's cost; the shape is identical in all six. The
      three largest bands are the substrate re-sampled per terrain vertex (63.0 ms), the cloud march
      (23.2 ms) and water's foam field — all three of them work a shipping engine would do in a
      shader, and all three are the GPU debts M10's own module READMEs record
- [x] 7.4 **Capture it.** Anything with a visible result gets an image under `docs/design/images/`,
      and a diagram is labelled one.
      **Done**: `just capture-world` writes `docs/design/images/m10-world.png`,
      `docs/design/images/m10-world-budget.png` and `docs/design/videos/m10-world.mp4` from nothing
      but a seed, through `tools/docs/collect_world.py`

## 7b. The visibility buffer's depth/payload race — done before M10 opened

Found by rendering the M7 scene for documentation and fixed in the same pass; kept here because the
milestone's records should say where it went. See `docs/roadmap/post-m9-tasks.md` §4.

- [x] 7b.1 Depth and payload settled in one 64-bit atomic min, with `vg.vis.unpack` deriving the
      visibility buffer so no downstream pass changed
- [x] 7b.2 **Regression test** — six runs over overlapping instances, resolve and bin counts required
      to match; verified to fail on the old raster
- [x] 7b.3 The remaining residue: exact depth ties broken by traversal append order, 1-3 pixels in
      921,593. Needs a stable cluster identity in the raster payload rather than the visible index.
      **Done in code.** The raster payload's first word is `instance * cluster_stride + cluster` —
      the traversal's own DAG mark index — instead of the visible-list slot, and classify, scatter
      and resolve derive the material from it rather than indexing that list, so no pass after the
      raster reads a number produced by an atomic append. **IT FITS EXACTLY AND THE ARITHMETIC IS
      WRITTEN DOWN**: the atomic is 64 bits, 32 of depth key and 32 of payload of which 8 are the
      triangle, so the identity takes the SAME 24 bits the visible index had — but the scene bound
      moves from `visible_capacity <= 2^24` to `instance_count * cluster_stride <= 2^24`, which is
      four times tighter than the 2^26 the traversal's own visit marks allow.
      `VisbufferPass::initialise` refuses above it by name and a test proves the boundary.
      **MEASURED**: three identical runs of `cy_fidelity_capture` at 1280x720 now agree on 921,600
      of 921,600 pixels in all three views; against the old raster the same comparison reports 1
      shaded pixel, 2-5 triangle pixels and 883,921-886,414 cluster pixels differing.
      **A SECOND DEFECT FELL OUT**: both `VisbufferPass` and `GpuTraversal` destroyed handles their
      refused `initialise` had never created — twelve "destroy_buffer() on a stale or never-issued
      handle" the first time a test destroyed a refused pass. Both destructors skip a null handle
      and the new test asserts the validation count
- [x] 7b.4 Tighten `fidelity.py`'s `materials_seen` assertion, which asks only for `> 1` and so
      never saw this.
      **Done in code.** Two checks replace the floor of one: the number is compared against
      `materials_placed`, which the program now computes from every cluster's material plus its
      instance's offset — the set the scene PLACES, whether drawn or not — and a new act compares
      coverage, the visible-cluster count and the bin count across every `--repeat` run, which
      nothing did before because the acts only ever ran on the first. Proved red by dropping one bin
      on odd-pid runs: `materials_seen took 2 values over 4 runs: 4, 5`, which is the flip itself

## 8. Records and gates

- [x] 8.1 `tools/roadmap/milestones/m10.toml`; declare `milestone-m10` in `gates.toml` and raise
      `selftest.MINIMUM_CRITERIA`
- [x] 8.2 An `m11-open` criterion using the double-star glob form
- [x] 8.3 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`.
      **Done.** Eight rows to Working (seven from `none`, `vfx-system` from Seed),
      `diagnostics-profiling-and-crash` and `gameplay-framework` to Complete, and
      `save-and-persistence` recorded at Working with its Complete cell moved to M11 — its second
      demotion, with the eleven-piece audit carried into M11's proposal as design.md §4 requires.
      **`atmosphere-sky-and-clouds` is recorded at Working with `m10:sky-field-round-trip` running
      and failing against it**, and the argument is written into `status.yaml` and into
      `capability-matrix.md#where-m10s-tiers-are-thin` rather than left implicit: one requirement of
      thirteen, and half of that one, against twelve built and measured by five criteria over four
      suites — and the row does NOT reach Complete at M11 while the gap is open.
      **Three parked Complete cells moved to M11**, each with a first-hand refutation rather than
      only an absence of work: nothing in the tree constructs a `gi::SkyTerm` from the atmosphere, so
      the seam `dependencies.md` cycle 2 is entirely about was never joined; `samples/10-world` has
      no streaming in it at all; and terrain's navigation contribution is one requirement of sixteen.
      **M11's load goes 61 → 65.** `ROADMAP.md`'s three false VFX sentences are corrected — the GPU
      path exists, `device_dispatch_available()` answers true and `DeviceDispatchUnimplemented` no
      longer exists as a reason — and the sweep found four more of the same kind, all fixed: M9's two
      gaps that M10 closed still reading as open, M5's paragraph claiming a plan/record divergence the
      audit closed, M6's gate never recording `developer-workflow-and-just`, and
      `docs/roadmap/implementing.md`'s "In flight" section still naming **M5.5**, five rungs stale.
      `docs/roadmap/open-debts.md` regenerated; `just roadmap-test` 214/214, `just roadmap-status`,
      `just roadmap-debts --check`, `just ci-check` and `just quality-specs` all clean
- [ ] 8.4 Move `ci.yml`'s milestone job to `m10` in the same commit that flips the gate green.
      **HALF DONE, AND THE OTHER HALF IS NOT THIS PHASE'S TO DO.** `.github/workflows/ci.yml`'s
      `milestone` job runs `just roadmap-milestone m10 --ci`, and its comment block records what M10
      adds: four declared gaps that run and fail by design, two `where = "ci"` criteria that defer to
      *this job* and still fail there because no leg-comparison job exists, and two `requires = "gpu"`
      criteria each with a device-free companion. **`tools/roadmap/gates.toml` is deliberately still
      at `state = "joins-on-close"`**: flipping it asserts the gate is green, and section 9 has not
      run. Both states were measured rather than assumed — with the gate green and `ci.yml` at `m10`,
      `just ci-check` exits 0; with the gate green and `ci.yml` back at `m9` it fails naming
      `milestone-m10`, which is the forcing function this task relies on. **This commit must not land
      before the gate is green**, because the job runs on every push to `main`
- [x] 8.5 Open the M11 change.
      **Done**: `openspec/changes/implement-m11-reach/` — README, proposal and tasks.
      `m10:m11-open` passes, and was proved red twice (proposal moved away; directory renamed so the
      glob no longer matches). The proposal carries what M10 hands forward as six running, failing,
      rung-bearing criteria plus the two questions this host cannot ask, the
      `save-and-persistence` mis-scoping finding design.md §4 sends here, and the decision M11 cannot
      avoid: **whether it is one milestone or two**, now that its load is 65 of 76 capabilities with
      seventeen of them arriving at M10's gate alone. `tasks.md` is a decision list, not an
      implementation plan, and section 0 has to be answered before the rest can be written honestly.
      Like `implement-m10-worlds` at the equivalent point, it carries no `specs/` deltas yet, so
      `openspec validate --changes --strict` reports it as having no delta — the same state M10's
      change was opened in, and nothing in CI validates changes

## 9. The gate

- [ ] 9.1 Clean build of every profile from empty; `test-all` in each; every gate by hand
- [ ] 9.2 **Every criterion executes something and can fail** — break what it checks and prove it goes
      red
- [ ] 9.3 Adversarial pass on M10's own invariants: a second producer for one field; a region
      regenerated from the same seed; a hand-placed override across a regeneration; the environment
      budget across a full cycle
- [ ] 9.4 Records verified against what the code supports, **including the M10 column's own Complete
      cells against the status record** — `m9:record-matches-plan` is the check M9's gate added for
      exactly this and it is not milestone-specific in shape
