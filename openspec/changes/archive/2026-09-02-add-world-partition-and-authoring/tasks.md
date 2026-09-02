# Tasks: CyberWorld and the authoring model

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog and belong to
future changes, sequenced by the milestone table in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: the `World` naming rule, the four independent axes (cell
      residency, cell activation, asset residency, simulation detail), cell-relative coordinates
      as the reconciliation with the existing precision policy, authoring partition versus runtime
      partition, one file per authoring unit with the arithmetic against per-entity files, the
      transactional-but-incremental activation tension and its resolution, reference policies and
      dependency explosion, the persistence overlay, prefabs as authoring versus entity templates
      as runtime, field classification, exposed parameters as a prefab's public interface, the
      override-conflict correction, world HLOD versus virtual geometry, representation tiers, and
      the milestone table
- [x] 1.2 New `world-partition-and-streaming` capability: 32 requirements covering the persistent
      world layer, world asset, coordinates, partitioner, cell identity, spatial binding and
      policy, large entities, subsystem payloads, ECS-native cooking, residency and activation
      states, streaming sources, shapes and prediction, channels, priority and budgets, staged
      activation, lifecycle events, persistent identity, cross-cell references, dependency
      explosion detection, layers and scenario switching, world HLOD, dynamic migration,
      persistence overlay, cost model, client and server profiles, representation tiers, subsystem
      contracts, cooking, validation, diagnostics, and the gameplay API
- [x] 1.3 `serialization-and-prefabs` extended into the full authoring model: asset kinds, scene
      instances and cook modes, exposed parameters, stable override addressing, entity templates
      and batch spawning, hierarchy flattening, cycle rejection, field classification, live prefab
      update, authoring file granularity, and diff and provenance
- [x] 1.4 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 **Four workarounds removed.** `networking-and-replication` replication cells now derive
      from the world partition instead of being networking-owned; `virtual-geometry` streaming
      seams now name their owner and gain the guarantee that an activated cell is never blank;
      `rendering-global-illumination` GI scene is cell-scoped against a capability that now exists;
      renderer profiles at open-world scale have a system to point at.
- [x] 2.2 `core-math` — the precision policy is strengthened rather than reversed: cell-relative
      coordinates are the authoritative persistent form, 64-bit is tooling and interchange only
- [x] 2.3 `ecs-core` — the `World` naming rule is stated where `World` is defined, and bulk entity
      creation from prepared archetype blocks is required, since that is how cells and templates
      instantiate
- [x] 2.4 `navigation` — consumes cell lifecycle events and is cooked as a cell channel, while
      keeping its own tile layout independent of cell boundaries
- [x] 2.5 `editor-architecture` — world outliner over a metadata index, inspection without
      activation, the streaming debugger, structural prefab diff, and override provenance
- [x] 2.6 `serialization-and-prefabs` — **an earlier decision corrected**: a stale override is no
      longer dropped with a warning but becomes an explicit conflict with declared resolutions.
      Silently discarding deliberate designer work in a shipping build was the wrong default.
- [x] 2.7 `thirdparty-dependencies` — the persistent world and the prefab compiler recorded as
      engine-built, with an external world framework evaluated against the coupling it cannot
      provide
- [x] 2.8 `core-assets-and-io`, `physics`, `audio`, `ai-system`, `vfx-system` — reviewed; no change
      needed. Asset streaming already owns residency and the world coordinates with it rather than
      competing; physics and audio consume cell payloads through the contracts table; AI LOD
      already specifies a statistical population model, and representation tiers are the mechanism
      that feeds it rather than a replacement for it.
- [x] 2.9 **Gaps recorded, not closed**: virtual textures, virtual shadow maps, and procedural or
      runtime-generated worlds (the partitioner interface admits them; authoring, cooking, and
      identity for content that does not exist at cook time are not specified)

## 3. M1–M2 — the foundation (deferred)

- [ ] 3.1 World coordinates and the cell-relative location type
- [ ] 3.2 Persistent entity identity and the registry
- [ ] 3.3 World asset, cell index, hierarchical grid partitioner, stable cell identity
- [ ] 3.4 Cell cooking to archetype blocks matching runtime chunk layout
- [ ] 3.5 Bulk entity creation in `World` from prepared blocks
- [ ] 3.6 Load, unload, activate, deactivate with the full state machine
- [ ] 3.7 Benchmark activation of a large cell against the per-entity construction baseline

## 4. M3 — streaming (deferred)

- [ ] 4.1 Streaming sources with shapes and prediction
- [ ] 4.2 Central priority planner with deadlines
- [ ] 4.3 Streaming channels
- [ ] 4.4 I/O, memory, and activation budgets coordinated with asset residency
- [ ] 4.5 Staged activation with atomic publication
- [ ] 4.6 Cell lifecycle events with declared consumer ordering

## 5. M4–M6 — authoring (deferred)

- [ ] 5.1 Prefab compiler producing entity templates; batch spawning
- [ ] 5.2 Override addressing by stable identifiers; conflict states and resolutions
- [ ] 5.3 Exposed parameters with multi-field binding
- [ ] 5.4 Hierarchy flattening with per-entity override
- [ ] 5.5 Scene assets, scene instances, embedded and packed cook modes
- [ ] 5.6 Structural diff and provenance in the data model

## 6. M7–M9 (deferred)

- [ ] 6.1 World layers, layer states, scenario switching
- [ ] 6.2 Cross-cell references with policies; dependency explosion analysis in the cook report
- [ ] 6.3 Persistence overlay; saves, server persistence, replays, play-mode changes
- [ ] 6.4 World HLOD generation and the streaming-driven swap
- [ ] 6.5 Predictive streaming from paths, velocity, and cinematic tracks

## 7. M10–M11 (deferred)

- [ ] 7.1 Replication cells derived from world cells; server streaming hints to clients
- [ ] 7.2 Server and client cook and runtime profiles
- [ ] 7.3 Representation tiers with promotion and demotion preserving state
- [ ] 7.4 Authoring chunks, rebalancing, collaboration workflow
- [ ] 7.5 Streaming debugger and world profiler
- [ ] 7.6 Field classification and live prefab update in development builds

## 8. Validation (deferred)

- [ ] 8.1 Activation cost benchmarks: bulk copy versus per-entity construction, as a regression
      guard
- [ ] 8.2 No partial observation: a test asserting systems never see a half-activated cell
- [ ] 8.3 Identity stability: entities moved between authoring chunks, prefabs edited, worlds
      repartitioned — persistent identity unchanged and references intact
- [ ] 8.4 Save compatibility across content rebuilds with unchanged partition settings
- [ ] 8.5 Dependency explosion detection on a deliberately pathological world
- [ ] 8.6 Streaming determinism: the same source path produces the same request sequence
- [ ] 8.7 Precision: entity behaviour at 1 000 km from the origin matches behaviour at the origin
- [ ] 8.8 Override conflict handling: no override is lost in any build configuration
- [ ] 8.9 Client and server profiles: identical cell, entity, and layer identity from one source

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `world-partition-and-streaming` is a new
capability (32 requirements), `serialization-and-prefabs` is extended into the full authoring model
(20 requirements), and eight other capabilities were updated — four of which were carrying explicit
workarounds for this capability's absence. The unchecked items from section 3 onward are the
implementation backlog, sequenced by the milestone table in `design.md`; **M2 is the milestone that
matters** — if activation is not a bulk copy, no amount of scheduling hides it.
