# Tasks: M10 — Worlds

Ordered. Section 0 is the spike and it runs first because region invalidation decides the shape of
every producer that writes into the substrate, and section 1 is the substrate because seven of this
milestone's eight rows write into it.

## 0. The spike — region invalidation in PCG

- [ ] 0.1 Measure, do not assume: a dependency-driven partial regeneration over a graph with known
      edges, against a full-world regeneration of the same seed, compared for identity of output and
      of generated identity. A partial regeneration that does not reproduce the full one is the
      finding, and it is a finding about the graph's dependency model rather than about the seed
- [ ] 0.2 State plainly what the spike cannot answer on this host, through the ledger's own
      `requires`/`where` mechanism rather than as a sentence
- [ ] 0.3 Commit the spike and record its answer in `design.md`, the way M8.b's IR spike and M9's
      determinism spike were committed and consumed without re-deriving them

## 1. The substrate — one producer per field

- [ ] 1.1 Field declaration and the sparse tiled store; CPU and GPU access to the same field
- [ ] 1.2 **One producer per field, refused at registration** — a second producer for the same field
      fails, naming both producers. The exit criterion says "a second producer registration fails",
      so the refusal is the criterion and it must be shown to fail with the check removed
- [ ] 1.3 Residency levels and streaming, against `world-partition-and-streaming`'s existing cells
- [ ] 1.4 Determinism of gameplay-visible fields, and the firewall for presentation-only ones —
      `simulation-and-determinism`'s classification, not a second mechanism beside it

## 2. Terrain, water, foliage

- [ ] 2.1 Tiled hierarchical storage; terrain as a geometry source; material layers and frequency
      separation
- [ ] 2.2 Deformation classes and deltas through the persistence overlay; collision and the
      navigation contribution; HLOD and the modifier stack
- [ ] 2.3 Water bodies, the displacement contract, spectral ocean, rivers, shoreline, surface and
      underwater shading, foam, caustics, queries, buoyancy
- [ ] 2.4 Foliage instances that are not entities, clusters, promotion, deterministic procedural
      placement, GPU grass, wind response, the interaction field, the budget

## 3. Weather, wind, atmosphere

- [ ] 3.1 Climate and weather cells, environment sampling, the wind field
- [ ] 3.2 Precipitation, wetness and snow, storms, presets and transitions, ecosystem state
- [ ] 3.3 The physical atmosphere and its tables, aerial perspective, celestial bodies, volumetric
      clouds and their shadows, planetary scale

## 4. Procedural content generation

- [ ] 4.1 Typed datasets, compiled graphs, execution domains, deterministic derivation
- [ ] 4.2 **Stable generated identity**, and a hand-placed override that survives regeneration of its
      region — both are exit criteria and both are tests before they are prose
- [ ] 4.3 Regions and spatial invalidation from section 0's answer; caching; output adapters;
      provenance; persistence of generated content

## 5. `vfx-system` → Working

- [ ] 5.1 Particle state resident in GPU buffers; indirect dispatch driven by GPU-maintained counts
- [ ] 5.2 Async compute where the device exposes a queue; the GPU sort behind `BudgetLevers::sorted`
- [ ] 5.3 The renderer kinds beyond `Sprite` and `Mesh`

## 6. The three rows M9 demoted, and the gaps it declared

- [ ] 6.1 **`diagnostics-profiling-and-crash` → Complete**: adopt `CY_BREADCRUMB` at the five
      boundaries the specification names, and repair `m9:crash-artefact-paths` — a module table
      captured at install time through `dl_iterate_phdr`, with the fault path writing addresses plus
      a basename. Both criteria are declared gaps that CLOSE here, and a declared gap that starts
      passing fails the ledger, so closing them is checked rather than claimed
- [ ] 6.2 **`save-and-persistence` → Complete**: confidentiality over the existing container — a
      vetted AEAD, which is a dependency decision through `thirdparty-dependencies` rather than a
      coding task — and conflict resolution
- [ ] 6.3 **`gameplay-framework` → Complete**: benchmark the specification's performance table
      rather than assert it, closing `m9:gameplay-benchmarks`
- [ ] 6.4 **`tests/determinism/` stops being empty.** The suite `testing-and-quality` gives a
      location and a 10 s budget: golden replays with committed hashes replayed in CI, replay and
      save fuzzing, and the transactional save tests. `cy_add_test`'s taxonomy gains the kind its own
      error message already promises
- [ ] 6.5 **`m9:record-matches-plan-history`**: audit the four closed milestones whose matrix columns
      claim nineteen cells the status record does not support, thirteen of them Complete, and move
      or claim each one. An OpenSpec change against `delivery-roadmap`, because moving a capability
      between milestones is one by that specification's own rule

## 7. The artefact — `samples/10-world`

- [ ] 7.1 Procedurally populated terrain with rivers and an ocean; foliage responding to a wind field
      driven by weather; wetness and snow accumulating; a full day/night cycle with volumetric clouds
- [ ] 7.2 Streamed and persistent: the world saves, resumes and patches, against M6's own artefact
- [ ] 7.3 The environment demo holds its frame budget across a full day/night cycle, **measured as a
      curve across the cycle** rather than asserted at one time of day
- [ ] 7.4 **Capture it.** Anything with a visible result gets an image under `docs/design/images/`,
      and a diagram is labelled one

## 7b. The visibility buffer's depth/payload race — done before M10 opened

Found by rendering the M7 scene for documentation and fixed in the same pass; kept here because the
milestone's records should say where it went. See `docs/roadmap/post-m9-tasks.md` §4.

- [x] 7b.1 Depth and payload settled in one 64-bit atomic min, with `vg.vis.unpack` deriving the
      visibility buffer so no downstream pass changed
- [x] 7b.2 **Regression test** — six runs over overlapping instances, resolve and bin counts required
      to match; verified to fail on the old raster
- [ ] 7b.3 The remaining residue: exact depth ties broken by traversal append order, 1-3 pixels in
      921,593. Needs a stable cluster identity in the raster payload rather than the visible index
- [ ] 7b.4 Tighten `fidelity.py`'s `materials_seen` assertion, which asks only for `> 1` and so
      never saw this

## 8. Records and gates

- [ ] 8.1 `tools/roadmap/milestones/m10.toml`; declare `milestone-m10` in `gates.toml` and raise
      `selftest.MINIMUM_CRITERIA`
- [ ] 8.2 An `m11-open` criterion using the double-star glob form
- [ ] 8.3 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`
- [ ] 8.4 Move `ci.yml`'s milestone job to `m10` in the same commit that flips the gate green
- [ ] 8.5 Open the M11 change

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
