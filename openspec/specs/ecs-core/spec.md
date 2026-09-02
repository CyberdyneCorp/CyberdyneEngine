# ecs-core Specification

## Purpose

Defines the entity-component-system core: the authoritative runtime data model. Entities are
generational ids, component data lives in packed per-archetype chunks, and behaviour is
expressed as systems over queries, scheduled onto the job system.

The archetype model is drawn from Unity DOTS; the staged, deterministic scheduler and the
explicit access declarations are chosen so that parallelism is safe by construction rather than
by review.

## Requirements

### Requirement: Entities
An `Entity` SHALL be a 64-bit value packing a 32-bit index and a 32-bit generation. Entity ids
SHALL be dense and recycled, with the generation incremented on destruction so stale ids are
detectable.

The `World` SHALL own entity allocation and SHALL provide `create`, `destroy`, `is_alive`, and
bulk variants `create_many` and `destroy_many`.

#### Scenario: Stale entity id
- **WHEN** an entity is destroyed and its index reused
- **THEN** `is_alive` on the old id SHALL return false and component access through it SHALL
  return null

#### Scenario: Bulk creation is cheap
- **WHEN** 100 000 entities are created with the same component set
- **THEN** they SHALL be allocated into chunks of one archetype in bulk, without per-entity
  archetype lookup

### Requirement: Components
A component SHALL be a reflected struct with no virtual functions, trivially relocatable, and
storable in packed arrays.

The engine SHALL support these component kinds:

| Kind | Storage | Use |
|---|---|---|
| Data component | Packed SoA array in the archetype chunk | Ordinary per-entity data |
| Tag component | Zero-sized; presence only, part of the archetype key | Marking and filtering |
| Shared component | Interned value; entities sharing it group into the same chunk | Render material, LOD group |
| Buffer component | Variable-length array per entity, inline up to a capacity then heap | Waypoints, inventory |
| Sparse component | Side table keyed by entity, no archetype change | Rarely present, frequently toggled data |

Adding or removing a data, tag, or shared component SHALL change the entity's archetype and move
its data; adding or removing a sparse component SHALL NOT.

#### Scenario: Choosing sparse over data
- **WHEN** a component is present on under ~1 % of entities and toggles frequently
- **THEN** it SHOULD be declared sparse, avoiding archetype churn on every toggle

#### Scenario: Shared component groups chunks
- **WHEN** many entities share the same render material
- **THEN** they SHALL be grouped into chunks by that shared value, so rendering can submit whole
  chunks without per-entity material lookup

### Requirement: Archetypes and chunk storage
An **archetype** SHALL be the unique set of component types on an entity. All entities of one
archetype SHALL be stored in chunks (default 16 KiB) holding structure-of-arrays data.

Each chunk SHALL contain: a header (archetype pointer, entity count, capacity, per-component
change versions, shared component values), the entity id array, and one contiguous array per
component type, each aligned to its type's alignment.

Chunk capacity SHALL be derived from the archetype's total per-entity size.

#### Scenario: Iteration is contiguous
- **WHEN** a query iterates matching chunks
- **THEN** each component array SHALL be traversed linearly, and the compiler SHALL be able to
  vectorise the loop body

#### Scenario: Archetype transition
- **WHEN** a component is added to an entity
- **THEN** the entity's data SHALL be moved to a chunk of the target archetype and the vacated
  slot filled with the chunk's last entity, updating that entity's location record

#### Scenario: Component addition order does not matter
- **WHEN** two entities receive the same components in different orders
- **THEN** both SHALL end up in the same archetype

### Requirement: Queries
A `Query` SHALL select entities by component constraints: `With<T...>` (required, accessed),
`Without<T...>` (excluded), `Optional<T>` (accessed if present), plus `Read<T>` / `Write<T>`
access declarations.

Queries SHALL be **cached**: the set of matching archetypes SHALL be computed once and
incrementally updated as archetypes are created, not recomputed per frame.

Queries SHALL support **change filtering** — iterate only chunks whose component change version
exceeds the system's last-run version — and shared-component filtering.

#### Scenario: Query matching is amortised
- **WHEN** a query runs every frame in a world with a stable set of archetypes
- **THEN** matching SHALL cost only iteration of the cached archetype list

#### Scenario: Change filtering skips untouched data
- **WHEN** a system processes only entities whose `Transform` changed
- **THEN** chunks whose `Transform` version has not advanced since the system's last run SHALL be
  skipped entirely

#### Scenario: Random access
- **WHEN** a system needs a specific entity's component outside iteration
- **THEN** it SHALL use a lookup through the entity location table, documented as slower than
  iteration and inappropriate for bulk work

### Requirement: Systems and access declarations
A system SHALL declare its component access (`Read`, `Write`, `Exclude`), resource access, and
event channel access as part of its type, and SHALL be registered into a **stage**.

Stages SHALL be, in execution order: `PreSimulation`, `Physics`, `Simulation`,
`PostSimulation` (fixed step); then `Frame`, `Animation`, `UI`, `Render` (variable step).

Within a stage, the scheduler SHALL derive a dependency graph from access declarations and
execute non-conflicting systems in parallel on the job system. Explicit `before` / `after`
ordering constraints SHALL be supported where a semantic order is needed beyond data conflicts.

#### Scenario: Ordering is deterministic
- **WHEN** two systems have no data conflict and no explicit ordering
- **THEN** their relative order SHALL still be deterministic across runs, derived from a stable
  registration order

#### Scenario: Explicit ordering
- **WHEN** an input system must run before a movement system that reads its output resource
- **THEN** the dependency SHALL be expressed by the resource access, or by an explicit `before`
  constraint if no shared data exists

#### Scenario: Cyclic ordering is rejected
- **WHEN** explicit constraints form a cycle
- **THEN** registration SHALL fail at startup with a diagnostic naming the cycle

### Requirement: Structural change deferral
Systems SHALL NOT create or destroy entities, add or remove components, or otherwise mutate
archetypes during query iteration. They SHALL record such operations into a per-thread
`CommandBuffer`.

Command buffers SHALL be applied at the end of the stage, merged in a deterministic order, and
SHALL support: create entity (with an immediately usable placeholder id), destroy entity, add
component, remove component, set component, and add child.

#### Scenario: Spawn during iteration
- **WHEN** a system spawns a projectile while iterating weapons
- **THEN** the spawn SHALL be recorded and applied at the stage flush, and the placeholder entity
  id SHALL be remapped to the real id at that point

#### Scenario: Deterministic merge
- **WHEN** several worker threads record commands
- **THEN** the merge order SHALL be by system order then by thread index, so results are
  reproducible

### Requirement: Resources and singletons
The world SHALL hold **resources**: named, typed singleton values (time, input snapshot,
configuration, server handles) accessed by systems through declared `Read`/`Write` access, so
they participate in conflict detection like components.

#### Scenario: Resource conflict is scheduled
- **WHEN** two systems write the same resource
- **THEN** the scheduler SHALL serialise them

### Requirement: Change detection and versioning
The world SHALL maintain a monotonically increasing global version incremented per stage. Each
chunk SHALL record, per component type, the version at which it was last written.

Writing through a `Write<T>` accessor SHALL bump the chunk's version for `T`. Read-only access
SHALL NOT.

#### Scenario: Read does not dirty
- **WHEN** a system reads `Transform` without writing
- **THEN** the chunk's `Transform` version SHALL be unchanged and downstream change filters SHALL
  not fire

#### Scenario: Conservative granularity
- **WHEN** one entity in a chunk is written
- **THEN** the whole chunk SHALL be considered changed; this is documented as chunk-granular, not
  entity-granular

### Requirement: Entity relationships
The world SHALL support parent-child relationships as a first-class relation, maintained as
`Parent` and `Children` components kept consistent by the world rather than by user code.

Destroying a parent SHALL destroy its descendants by default, with an opt-out that reparents
them to the destroyed entity's parent.

#### Scenario: Hierarchy consistency
- **WHEN** an entity is reparented
- **THEN** both the old and new parents' `Children` and the entity's `Parent` SHALL be updated
  atomically at the flush point

#### Scenario: Cascade destroy
- **WHEN** a parent is destroyed
- **THEN** its entire subtree SHALL be destroyed in one deferred operation

### Requirement: World serialization and snapshots
The world SHALL support serializing all or a filtered subset of entities and components to the
binary or text format, with entity references remapped to stable local ids.

The world SHALL support fast in-memory **snapshots** for rollback, editor play-mode reset, and
deterministic testing.

#### Scenario: Play-mode reset
- **WHEN** the editor exits play mode
- **THEN** the world SHALL be restored from the snapshot taken on entry, with no reliance on
  systems undoing their own mutations

#### Scenario: Entity references survive round-trip
- **WHEN** a component holding an `Entity` reference is serialized and reloaded
- **THEN** the reference SHALL resolve to the corresponding reloaded entity

### Requirement: Multiple worlds
The engine SHALL support multiple concurrently existing `World` instances — for example the
edited world and the play-mode world, or a client-prediction world and a server-authoritative
one — each with independent entities, archetypes, and schedules.

#### Scenario: Editor and play-mode coexist
- **WHEN** the editor enters play mode
- **THEN** the played world SHALL be a separate `World` instance so editing state is untouched

### Requirement: ECS diagnostics
Development builds SHALL expose: archetype and chunk counts with fill ratios; per-system
execution time and entity counts; query match statistics; structural-change counts per frame;
and detection of archetype thrash (an entity changing archetype more than a threshold per
second).

#### Scenario: Archetype thrash warning
- **WHEN** entities repeatedly gain and lose a component every frame
- **THEN** a diagnostic SHALL identify the component and suggest declaring it sparse
