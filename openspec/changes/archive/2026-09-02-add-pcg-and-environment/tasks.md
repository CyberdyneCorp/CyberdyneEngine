# Tasks: CyberPCG and CyberEnvironment

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog, sequenced by the
build order in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: the substrate waiting for producers, procedural generation already
      happening in four places, typed datasets rather than an object spawner, output as the
      consumer's representation, stable identity as the thing that makes the workflow survivable,
      region-based dependency-driven invalidation, explainability as a requirement, weather
      publishing state rather than touching anything, the three layers, wind as the field with the
      most consumers and no producer, clouds reconstructed rather than stored, precipitation without
      drop simulation, potential versus state, macro state materialised on demand, per-generator
      determinism, and the build order
- [x] 1.2 New `procedural-content-generation` (22 requirements): typed datasets, compiled graphs,
      execution domains, deterministic derivation, stable generated identity, regions, dependencies
      and spatial invalidation, caching and distribution, output adapters, field integration,
      overrides and regeneration, provenance, CPU and GPU execution, spatial queries, bounded
      iteration, runtime generation, macro state and materialisation, persistence, networking,
      diagnostics, performance, and forbidden patterns
- [x] 1.3 New `atmosphere-sky-and-clouds` (13 requirements): physical atmosphere, precomputed
      tables, aerial perspective, celestial bodies and time of day, sky composition, cloud
      representation and rendering, cloud shadows, volumetric integration, planetary scale,
      environment profiles, quality tiers, and diagnostics
- [x] 1.4 New `weather-and-wind` (16 requirements): weather publishes state, the three layers,
      weather cells, environment sampling, the wind field, precipitation, wetness and snow, storms
      and lightning, presets and transitions, determinism and the firewall, replay and networking,
      ecosystem state, budgets, diagnostics, time simulation tooling, and forbidden patterns
- [x] 1.5 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 **Two recorded gaps close.** Weather was recorded as "specified as a field producer against
      a system that does not exist", and procedural generation beyond foliage rules was named as
      missing. The wind field had six consumers and no producer; it now has one.
- [x] 2.2 `environment-fields` — residency levels so macro state exists world-wide while detail
      follows importance; region-granular versioning and change events so derived results invalidate
      precisely; and the **potential versus current state** distinction, which is what makes an
      ecosystem that recovers rather than a map that is painted once. The wind field gains its
      declared producer.
- [x] 2.3 `terrain` — the modifier stack becomes a consumer of the procedural framework rather than
      a parallel procedural system, with regional caching and radius-scoped invalidation
- [x] 2.4 `foliage` — placement rules become procedural programs producing foliage populations
      through the output adapter, and materialisation respects macro ecosystem state
- [x] 2.5 `rendering-global-illumination` — sky and atmosphere move to their own capability while
      illumination keeps what it consumes, with incremental update on continuous sky change and
      cloud shadowing through the coarse field rather than shadow pages
- [x] 2.6 `water` — the weather seam closes; hydrology remains open and is now explicitly the last
      environmental gap, with its inputs and outputs already expressed as fields
- [x] 2.7 `thirdparty-dependencies` — the procedural framework and the environment architecture
      recorded as engine-built, with scattering and cloud rendering following published research and
      noise, sampling and transform libraries integrated beneath
- [x] 2.8 `vfx-system`, `audio`, `ai-system`, `virtual-texturing`, `residency`,
      `simulation-and-determinism` — reviewed; no change needed. Wind and environment sampling reach
      them through the field substrate, the runtime virtual texture path already composites
      world-state layers, and the determinism firewall already classifies GPU-produced state as
      presentation.
- [x] 2.9 **Remaining gap**: hydrology and erosion

## 3. Phase 1–2 — substrate and generation core (deferred)

- [ ] 3.1 Field residency levels; region versioning and change events; potential and state fields
- [ ] 3.2 Typed spatial datasets with compiled attribute identifiers
- [ ] 3.3 Deterministic derivation and stable generated identity
- [ ] 3.4 Region execution model with per-generator region sizing
- [ ] 3.5 Graph authoring, intermediate representation, and compiler with fusion and elimination

## 4. Phase 3–5 — outputs, incrementality, tooling (deferred)

- [ ] 4.1 Output adapters: terrain stamps, foliage populations, field tiles, entity templates,
      splines
- [ ] 4.2 Dependency declarations with sampling radius; dirty set computation
- [ ] 4.3 Derivation keys and derived data cache integration; distribution across build workers
- [ ] 4.4 Override layer with identity merge and orphan reporting
- [ ] 4.5 Provenance capture; "why is this here" and "why is nothing here"
- [ ] 4.6 Graph editor, region debugger, dependency view, generation profiler

## 5. Phase 6–8 — atmosphere and weather state (deferred)

- [ ] 5.1 Atmosphere model and precomputed tables with incremental update
- [ ] 5.2 Celestial model and time of day on a declared time domain
- [ ] 5.3 Sky composition; radiance and irradiance for illumination
- [ ] 5.4 Weather cells, hierarchical state, advection, terrain influence
- [ ] 5.5 The wind field: prevailing, weather, terrain, volumes, transients
- [ ] 5.6 Precipitation with occlusion representation; wetness and snow accumulation and decay

## 6. Phase 9–11 — clouds, storms, ecosystems (deferred)

- [ ] 6.1 Volumetric clouds from weather maps with temporal reconstruction
- [ ] 6.2 Coarse cloud shadow field consumed by surfaces and illumination
- [ ] 6.3 Storm phenomena, lightning events, weather presets and transitions
- [ ] 6.4 Determinism classification, replay and network integration
- [ ] 6.5 Macro ecosystem state and biome thresholds
- [ ] 6.6 On-demand materialisation consistent with macro state

## 7. Phase 12 — tooling and scale (deferred)

- [ ] 7.1 Environment inspector with contribution breakdown; wind, weather and field visualisation
- [ ] 7.2 Cloud debugger with coverage, steps, history confidence and shadow field
- [ ] 7.3 Time simulation tooling using the runtime macro model
- [ ] 7.4 Runtime generation with deadlines through the residency layer

## 8. Validation and benchmarks (deferred)

- [ ] 8.1 **Large-world generation benchmark**: a hundred-kilometre-square world with climate and
      biome fields, terrain analysis, roads, settlements, ten million trees and ground cover —
      modifying one road segment regenerates only dependent regions, the rest are cache hits, and
      author overrides survive
- [ ] 8.2 Determinism: identical generation across machines, worker counts and platforms for
      generators declaring it
- [ ] 8.3 Identity stability: regeneration after a rule change preserves surviving identities and
      reports orphans
- [ ] 8.4 No per-point allocation for million-candidate regions
- [ ] 8.5 **Dynamic environment benchmark**: a hundred-kilometre world with eight players and a
      storm crossing it — bounded weather simulation cost, high-resolution presentation only near
      viewers, wetness and moisture fields updating, and vegetation responding over time
- [ ] 8.6 Firewall: reducing cloud, volumetric or precipitation quality does not alter wind, rain
      intensity, visibility or any gameplay-visible value
- [ ] 8.7 Replay: a session with weather reconstructs from seed and recorded state, with no visual
      state recorded
- [ ] 8.8 Networking: a storm's bandwidth is its identity, motion and intensity, not cloud data
- [ ] 8.9 **Terraforming scenario**: fields altered by gameplay raise potential, current state
      follows, biome thresholds cross, and generation materialises vegetation consistent with the
      new state — end to end, with saves recording deltas rather than instances

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `procedural-content-generation`,
`atmosphere-sky-and-clouds` and `weather-and-wind` are in `openspec/specs/`, and six capabilities
were updated. **Two recorded gaps close** — weather, which had been specified as a field producer
against a system that did not exist, and procedural generation beyond foliage rules. The wind field
finally has a producer, and nothing that consumed it had to change. The unchecked items from section
3 onward are the implementation backlog; **phase 1, the field extensions, comes first** for the same
reason the substrate was specified before its consumers.
