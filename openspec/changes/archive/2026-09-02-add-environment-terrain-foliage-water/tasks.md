# Tasks: CyberEnvironment

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog and belong to
future changes, sequenced by the phase table in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: why the field substrate is specified beneath terrain rather than
      inside it, terrain as a geometry source with the heightfield honestly retained for
      non-rendering consumers, three representations with one required, the three deformation
      classes and their differing costs, terrain deltas reusing the persistence overlay, foliage
      promotion with identity preservation and refusable demotion, rules-plus-exceptions storage
      and the orphaned-exception failure mode, one wind field, the water displacement contract, one
      water abstraction over several backends, shoreline ownership keeping the terrain–water
      dependency acyclic, and the phase table
- [x] 1.2 New `environment-fields`: shared substrate, field declaration, one producer per field,
      sparse tiled streaming, CPU and GPU access, the wind field, runtime modification,
      determinism of gameplay-visible fields, diagnostics
- [x] 1.3 New `terrain`: three representations, tiled hierarchical storage, terrain as a geometry
      source, bounded material layers, frequency separation, environment-aware material inputs,
      deformation classes, deltas over the persistence overlay, collision, navigation
      contribution, hierarchical detail, the modifier stack, holes and decoration, queries,
      diagnostics
- [x] 1.4 New `foliage`: instances not entities, clusters, promotion and demotion, procedural
      placement, exceptions, GPU grass, geometry classes, wind response, interaction field,
      regional state, budget, authoring, diagnostics
- [x] 1.5 New `water`: bodies, backends, the displacement contract, ocean, rivers, shoreline,
      surface shading, underwater, foam, caustics, queries, buoyancy, navigation, streaming,
      weather and hydrology seams, authoring, diagnostics
- [x] 1.6 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `virtual-geometry` — terrain ceases to be a deferred deformation class; the seam that
      reserved "terrain produces virtual geometry rather than a duplicate representation" is
      closed by the terrain capability rather than reinterpreted
- [x] 2.2 `vfx-system` — wind comes from the shared field rather than a VFX-owned model, and
      environment fields join the data interface list; effect-local force fields stay VFX-owned
- [x] 2.3 `physics` — heightfield collision as a first-class shape with bulk registration and
      regional update, and buoyancy sampling the authoritative displacement bands
- [x] 2.4 `navigation` — terrain surface contribution, deformation dirty regions, water swimming
      and vessel navigation with directional cost, and field-driven traversal cost as data
- [x] 2.5 `world-partition-and-streaming` — environment payloads added to the contracts table, with
      the rule that a logical object spanning many cells is one entity with segmented runtime data
- [x] 2.6 `rendering-materials-and-shading` — a `Water` shading model, so water is not approximated
      as tinted opaque PBR
- [x] 2.7 `material-compiler` — environment-aware material inputs through the field substrate, with
      field usage recorded as a dependency and a missing field a cook-time diagnostic
- [x] 2.8 `editor-architecture` — terrain, foliage, water and field tools, non-destructive
      sculpting, and the requirement that a procedural result can explain itself
- [x] 2.9 `thirdparty-dependencies` — the environment systems recorded as engine-built, with wave
      synthesis, erosion and noise algorithms integrable beneath engine-owned interfaces
- [x] 2.10 `rendering-global-illumination`, `audio`, `ai-system` — reviewed; no change needed. GI
      already consumes geometry from the hierarchy and materials through secondary programs, and
      terrain and water participate as ordinary geometry; audio and AI consume fields through the
      substrate's public interface without needing changes of their own.
- [x] 2.11 **Gaps recorded, not closed**: **virtual textures** — terrain material is now the
      strongest case for them in the engine, and terrain is specified against them as a seam;
      **weather** — wind, precipitation and temperature are specified as field producers against a
      system that does not exist; **hydrology and erosion** — inputs and outputs are all fields;
      **procedural generation** beyond foliage rules; **virtual shadow maps**

## 3. Phase 1 — the foundation (deferred)

- [ ] 3.1 Field substrate: declaration, sparse tiles, streaming as a cell channel, CPU and GPU
      sampling
- [ ] 3.2 Tiled heightfield terrain with world partition integration
- [ ] 3.3 Terrain collision registered in bulk; height and material queries
- [ ] 3.4 Basic terrain material with bounded layers
- [ ] 3.5 Static foliage as GPU instances in clusters
- [ ] 3.6 Flat water bodies with queries and basic buoyancy

## 4. Phase 2 — the milestone that matters (deferred)

- [ ] 4.1 Terrain meshing to virtual geometry clusters, watertight across tiles and levels
- [ ] 4.2 Virtual terrain textures (blocked on virtual textures)
- [ ] 4.3 Hierarchical terrain streaming and macro representations
- [ ] 4.4 Biome and moisture fields with real producers
- [ ] 4.5 Deterministic procedural foliage placement from rules and seed
- [ ] 4.6 Wind field with layered contributions and its consumers

## 5. Phase 3 (deferred)

- [ ] 5.1 River spline networks, junctions, and flow fields
- [ ] 5.2 Shoreline computation writing wetness and water distance into fields
- [ ] 5.3 Water surface shading with absorption, refraction, and foam
- [ ] 5.4 Water queries and multi-point buoyancy against authoritative bands

## 6. Phase 4 (deferred)

- [ ] 6.1 Terrain runtime deformation by class, with collision and navigation invalidation
- [ ] 6.2 GPU-generated ground cover
- [ ] 6.3 Foliage interaction field, promotion and demotion with identity preservation
- [ ] 6.4 Shallow water backend

## 7. Phases 5 and 6 (deferred)

- [ ] 7.1 Cascaded spectral ocean; waterfalls
- [ ] 7.2 Underwater rendering and caustic tiers
- [ ] 7.3 Foliage destruction and regional environmental state
- [ ] 7.4 Signed distance field terrain, caves, runtime excavation
- [ ] 7.5 Erosion and hydrology (own change)

## 8. Validation (deferred)

- [ ] 8.1 Terrain watertightness across tile and detail boundaries
- [ ] 8.2 Deformation class tests: a visual deformation triggers no collision or navigation rebuild
- [ ] 8.3 Terrain delta round trip through save, reload, and replay
- [ ] 8.4 Foliage determinism: identical regeneration from seed and rule version across machines
- [ ] 8.5 Promotion round trip: fell a tree, demote, re-promote, and verify state survived
- [ ] 8.6 Orphaned exception handling after a rule change: nothing silently discarded
- [ ] 8.7 **Displacement contract test**: a floating body's height matches the rendered surface
      within the declared authoritative amplitude, under every ocean backend
- [ ] 8.8 Shoreline dependency test: terrain makes no call into water
- [ ] 8.9 Field conflict detection: two producers for one field fail at configuration time
- [ ] 8.10 Streaming tests: environment queries in unstreamed regions return defined coarse
      results and never block
- [ ] 8.11 Budget tests: foliage and terrain hold their allocations without visible pumping

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `environment-fields`, `terrain`, `foliage`
and `water` are in `openspec/specs/`, and nine existing capabilities were updated — including
`virtual-geometry`, where terrain stops being a deferred deformation class. The unchecked items
from section 3 onward are the implementation backlog, sequenced by the phase table in `design.md`;
**phase 2 is the milestone that matters** — phase 1 is a terrain system like any other engine's,
phase 2 is where terrain becomes geometry, foliage stops being hand-placed, and the field substrate
gains real producers.
