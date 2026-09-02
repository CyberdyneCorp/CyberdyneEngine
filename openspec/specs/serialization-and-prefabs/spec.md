# serialization-and-prefabs Specification

## Purpose

Defines how worlds, scenes, and prefabs are authored, stored, overridden, versioned, and compiled
into runtime data. It takes prefabs with per-instance overrides and nesting from Unity, scene
inheritance from Godot, and explicit reviewable text serialization from both — and then adds the
part neither provides: a compilation step.

Serialization is **two modes, not one format**. Tagged data — authoring, saves, overrides, replays
— is field-tagged, skip-unknown, migratable, and round-trippable. Cooked data — runtime assets and
ECS cell content — is packed with no per-field tags and loaded by bulk copy. Trying to satisfy both
with one format gives the worst of each; field-tagging a million transforms is the concrete failure
this rules out.

Everything addressable is addressed by **stable identity** (see `core-type-system`), which is what
lets a field be renamed without destroying the overrides on it, and lets migration operate on a
value record rather than on a version of a type that no longer exists in the code.

The governing idea for prefabs is that they are an authoring and compilation system, not a runtime
object hierarchy. Nesting, variants, and overrides resolve at cook time into immutable **entity
templates**, so spawning is a bulk copy; shipping builds carry no prefab link, while development
builds keep provenance and can update live.

## Requirements
### Requirement: Prefab, scene, and world are distinct asset kinds
The engine SHALL define three authoring asset kinds with distinct semantics, and SHALL NOT collapse
them into one:

| Kind | Meaning | Example |
|---|---|---|
| **Prefab** | A reusable object: one thing, instantiable many times | A robot, a door, a turret |
| **Scene** | A reusable spatial composition of prefabs and entities | A house interior, a factory block, a village |
| **World** | The top-level spatial universe, composed of scenes, prefab instances, and entities | A planet, a campaign map |

Prefabs and scenes MAY share a serialised representation, but the distinction SHALL be preserved in
the asset model, because the editor workflows, the cooking behaviour, and the runtime meaning
differ.

A world SHALL be a `world-partition-and-streaming` asset; scenes and prefabs SHALL be authoring
assets that a world composes.

#### Scenario: A village is a scene, not a prefab
- **WHEN** a designer builds a reusable settlement from many prefabs
- **THEN** it SHALL be a scene, instantiable in a world, rather than a prefab of prefabs

#### Scenario: Semantics are not interchangeable
- **WHEN** an asset kind is chosen
- **THEN** the editor SHALL offer the workflow appropriate to that kind rather than one generic
  container

### Requirement: Two serialization modes
Serialization SHALL have two modes with different guarantees, and neither SHALL be used for the
other's purpose:

| | **Tagged** | **Cooked** |
|---|---|---|
| Used for | Authoring data, saves, prefab overrides, replays, editor round trips | Runtime assets, ECS cell and chunk data, GPU-ready structures |
| Layout | Records of `FieldId`, wire type, length, payload | Packed, matching the runtime's in-memory layout |
| Evolution | Skips unknown fields, migrates, round-trips | None; cooker and runtime share one build |
| Cost | Per-field overhead, bounds-checked | Bulk copy with reference fixup |

Tagged data SHALL be: versioned, chunked, of defined endianness, bounds-checkable,
skip-unknown-capable, and streamable.

Cooked data SHALL carry **no per-field tags**. Field-tagging bulk runtime data — a million
transforms in a world cell — SHALL NOT occur.

Cooked data SHALL record the build schema identity it was produced against, and loading cooked
data produced by a different schema SHALL be a hard error, since it has no evolution mechanism.

#### Scenario: Bulk data is not tagged
- **WHEN** a world cell containing a million entities is cooked
- **THEN** its component data SHALL be packed archetype blocks with no per-field tags, loaded by
  bulk copy

#### Scenario: Saves survive code change
- **WHEN** a save written before a schema change is loaded
- **THEN** it SHALL be tagged data, migrated through the schema chain, and loaded successfully

#### Scenario: Cooked data mismatch is fatal
- **WHEN** cooked data's build schema identity does not match the runtime's
- **THEN** the load SHALL fail with a diagnostic, rather than reinterpreting packed bytes

### Requirement: Reflection-driven serialization
Serialization SHALL be driven entirely by the type registry: a type is serializable because it is
reflected, not because it implements a serializer.

Serialized data SHALL address fields by **`FieldId`** (see `core-type-system`), never by name or by
byte offset.

Fields marked `Transient` SHALL be skipped. Fields absent from serialized data SHALL take the
type's default value, so adding a field does not invalidate existing data.

Serialization code SHALL be **generated** per type rather than interpreted by walking field
descriptors at runtime, so that serializing many instances of a type is a loop over a known layout.

#### Scenario: New field is backward compatible
- **WHEN** a component gains a field and an older scene is loaded
- **THEN** the new field SHALL take its default and the load SHALL succeed

#### Scenario: Removed field is ignored
- **WHEN** serialized data contains a field the type no longer has
- **THEN** it SHALL be preserved as unknown data in tagged formats, and ignored in cooked formats,
  in neither case failing the load

#### Scenario: Unknown component is preserved
- **WHEN** a scene references a component type that is not registered (disabled module)
- **THEN** its data SHALL be retained as an opaque blob and written back unchanged on save

### Requirement: Field classification
Reflected fields SHALL declare a **classification** governing how they behave across live updates
and persistence:

| Class | Meaning |
|---|---|
| `Authoring` | Defined by the asset; updated when the asset changes |
| `RuntimeState` | Owned by the running simulation; preserved across asset updates; not persisted |
| `PersistentState` | Owned by the simulation and written to the persistence overlay |
| `Derived` | Computed; never serialised; recomputed on load and on change |

Classification SHALL be part of the type's reflected schema, so serialization, live update,
persistence, and networking all derive their behaviour from one declaration.

#### Scenario: Maximum and current health differ
- **WHEN** `Health.max` is `Authoring` and `Health.current` is `RuntimeState`
- **THEN** editing the prefab SHALL update the maximum and preserve the current value

#### Scenario: Derived data is not saved
- **WHEN** a field is classified `Derived`
- **THEN** it SHALL be absent from serialised data and recomputed on load

### Requirement: Text and binary forms
Scenes and prefabs SHALL have a canonical **text** form for source control and a **binary** form
for shipping, both produced from the same reflected data.

The text form SHALL be deterministic: stable ordering of entities, components, and fields;
no volatile data such as timestamps or pointer values; and a stable float formatting that
round-trips exactly.

#### Scenario: Minimal diff
- **WHEN** one property changes
- **THEN** the text diff SHALL show exactly one changed line

#### Scenario: Merge conflicts are tractable
- **WHEN** two developers add different entities to the same scene
- **THEN** the changes SHALL appear in separate regions of the file, so a textual merge succeeds

### Requirement: Authoring file granularity
Authoring data SHALL be stored **one file per authoring unit**: a prefab asset, a scene asset, or a
chunk of a world region containing hundreds to a few thousand entities.

One file per entity SHALL NOT be used. At the entity counts this engine targets it produces file
counts that version control systems and filesystems handle badly.

Because persistent identity is independent of file placement, entities SHALL be movable between
authoring chunks without changing identity, so chunk boundaries can be rebalanced without breaking
references.

Authoring chunk boundaries SHALL be independent of runtime cell boundaries.

#### Scenario: Two designers, no conflict
- **WHEN** two designers edit different parts of one region
- **THEN** they SHALL modify different authoring chunk files and merge without conflict

#### Scenario: Rebalancing is safe
- **WHEN** an authoring chunk grows too large and is split
- **THEN** entity identities SHALL be unchanged and all references SHALL still resolve

### Requirement: Entity references
References between entities within a scene SHALL be serialized as stable **local ids** assigned
at author time and preserved across saves, not as array indices or runtime entity ids.

References to entities in other scenes SHALL be serialized as scene `AssetId` plus local id, and
resolved when both are loaded.

#### Scenario: Reordering does not break references
- **WHEN** entities are reordered in the file
- **THEN** references SHALL still resolve, because they key on local id

#### Scenario: Cross-scene reference before load
- **WHEN** a cross-scene reference targets an unloaded scene
- **THEN** it SHALL resolve to a null handle and re-resolve if that scene is later loaded

### Requirement: Prefabs
A **prefab** SHALL be a serialized entity subtree that can be instantiated many times. An
instantiated prefab SHALL retain a link to its source asset.

Prefabs SHALL be **nestable**: a prefab may contain instances of other prefabs.

#### Scenario: Edit propagates to instances
- **WHEN** a prefab asset is modified
- **THEN** all instances SHALL reflect the change for every property they do not override

#### Scenario: Nested prefab
- **WHEN** prefab A contains an instance of prefab B and B is edited
- **THEN** the change SHALL propagate through A into A's instances

### Requirement: Instance overrides
A prefab instance SHALL store only its **differences** from the source: per-property value
overrides, added components, removed components, added child entities, and removed child
entities.

Each override SHALL be individually revertible, and the editor SHALL indicate which properties
are overridden.

An override whose target no longer exists SHALL become an explicit **override conflict**, not a
discarded value. Conflicts SHALL be retained in the authoring data, surfaced in the editor,
reported by validation, and resolvable by: discarding the override, retargeting it to another
field or entity, or restoring the removed structure onto the instance.

An override SHALL NOT be dropped silently in any build configuration. Silently discarding an
override discards work a designer did deliberately, in the one place where nobody is looking.

#### Scenario: Only the delta is stored
- **WHEN** an instance changes one material colour
- **THEN** the scene file SHALL record that single property override, not a copy of the subtree

#### Scenario: Revert to prefab
- **WHEN** an override is reverted
- **THEN** the property SHALL resume tracking the prefab value

#### Scenario: Property changed in both
- **WHEN** a property is overridden on an instance and later changed in the prefab
- **THEN** the instance SHALL keep its override, and the editor SHALL be able to surface the
  divergence

#### Scenario: Component removed in the prefab
- **WHEN** a component that an instance overrode is deleted from the prefab
- **THEN** the override SHALL become a conflict, retained and surfaced with its resolutions, and
  SHALL NOT be dropped

#### Scenario: Conflicts fail validation when configured
- **WHEN** unresolved override conflicts exist at cook time
- **THEN** validation SHALL report them and, by configuration, fail the build

### Requirement: Overrides address stable identifiers
An override SHALL address its target by **stable identifiers** — the prefab-local entity
identifier, the component type identifier, and the field identifier — and SHALL NOT be addressed by
a name path such as `Root.LeftArm.Weapon.Damage`.

Renaming an entity, a component, or a field SHALL NOT invalidate overrides; a field rename SHALL be
handled by the migration mechanism, which SHALL also migrate override targets.

Override operations SHALL be explicit and enumerable: set field, add component, remove component,
add entity, remove entity, and reparent entity.

#### Scenario: Renaming does not break overrides
- **WHEN** an entity inside a prefab is renamed
- **THEN** overrides targeting it SHALL continue to apply

#### Scenario: Field migration carries overrides
- **WHEN** a field is renamed and a migration is registered
- **THEN** overrides targeting the old field SHALL be migrated with the data

### Requirement: Prefab variants
A **variant** SHALL be a prefab whose base is another prefab, storing only its own overrides.
Variants SHALL be nestable.

Variant chains SHALL have a **configurable depth limit** with a recommended maximum, and exceeding
the recommendation SHALL warn: beyond a few levels, reasoning about where a value comes from
becomes impractical, and composition is the better tool.

The authoring model SHALL favour **composition over inheritance**: assembling a prefab from
component modules SHALL be the recommended way to express variation, with variants used for genuine
specialisation of one thing.

#### Scenario: Variant inherits later base changes
- **WHEN** the base prefab changes a property the variant does not override
- **THEN** the variant and its instances SHALL pick up the change

#### Scenario: Deep chains are discouraged
- **WHEN** a variant chain exceeds the recommended depth
- **THEN** the editor and the build SHALL warn, naming the chain

### Requirement: Exposed prefab parameters
A prefab SHALL be able to declare **exposed parameters**: a named, typed, documented set of values
forming the prefab's deliberate public interface.

An exposed parameter MAY drive several internal fields across several entities — a `Height`
parameter setting a pole's scale, a lamp's position, and a collider's extent — so that the
relationship is authored once rather than reproduced at every instance.

Instances SHALL be able to set exposed parameters. Arbitrary component overrides SHALL remain
available, but exposed parameters SHALL be the recommended path, because they let a prefab's
internals be refactored without breaking instances.

Parameters SHALL resolve to identifiers at cook time; runtime parameter application SHALL NOT
require string lookup.

#### Scenario: Internals stay private
- **WHEN** a prefab's internal entity structure is refactored while its exposed parameters are
  unchanged
- **THEN** existing instances SHALL continue to work without repair

#### Scenario: One parameter, many fields
- **WHEN** an instance sets `Height`
- **THEN** every field bound to that parameter SHALL be updated consistently

#### Scenario: Parameters are not strings at runtime
- **WHEN** a prefab is spawned with parameter values
- **THEN** parameters SHALL be applied by resolved identifier, not by name lookup

### Requirement: Dependency cycles are rejected
A prefab SHALL NOT contain, directly or transitively, an instance of itself. A scene SHALL NOT
contain, directly or transitively, an instance of itself. A variant chain SHALL NOT contain a
cycle.

Cycles SHALL be detected through the asset dependency graph and rejected at authoring time, with
the cycle reported as a chain.

#### Scenario: Cycle is rejected when created
- **WHEN** a designer attempts to place prefab A inside prefab B where B is already inside A
- **THEN** the operation SHALL be rejected with the cycle shown, rather than saved and failing at
  cook

### Requirement: Apply and extract
The editor SHALL support applying an instance's overrides back to its prefab source, and
extracting a subtree of a scene into a new prefab asset, replacing it in place with an instance.

#### Scenario: Apply overrides
- **WHEN** a designer applies an instance's overrides
- **THEN** the prefab asset SHALL be updated and the instance's override list cleared, with other
  instances picking up the change

#### Scenario: Extract to prefab
- **WHEN** a subtree is extracted
- **THEN** a new prefab asset SHALL be created and the scene SHALL contain an instance of it, with
  external references to the subtree rewritten to point at the instance

### Requirement: Prefab diff and override provenance
The data model SHALL support computing a **diff** between an instance and its base, between a
variant and its base, and between two versions of a prefab, expressed as added, removed, and
changed entities, components, and fields.

For any value, the model SHALL be able to report its **provenance**: whether it comes from the base
prefab, an intermediate variant, or an instance override, and what the inherited value is.

Provenance SHALL be part of the data model rather than an editor-only presentation, so it is
available to validation, review tooling, and diff output.

#### Scenario: Reviewing a change
- **WHEN** a prefab change is reviewed
- **THEN** the diff SHALL show added, removed, and changed structure rather than a raw text delta

#### Scenario: Where did this value come from
- **WHEN** a value on an instance is inspected
- **THEN** the model SHALL report whether it is inherited or overridden, and the inherited value

### Requirement: Scene instances and cook modes
A world SHALL be able to contain **scene instances**: a reference to a scene asset, a transform, an
instance identifier, and layer membership. One scene MAY be instanced many times.

Scenes SHALL be authored in **local coordinates**, with the instance transform placing them, so a
scene is reusable at any location.

Each scene instance SHALL declare a **cook mode**:

| Mode | Behaviour |
|---|---|
| `Embedded` | Entities are flattened into the world's cells, transforms baked into world space, and the instance ceases to exist at runtime |
| `Packed` | The instance is retained as a runtime unit with its own local space, streamed and owned as one |

`Packed` SHALL be used where the composition must move or be owned as a unit — a ship interior, a
train, an elevator, a procedurally placed room. `Embedded` SHALL be the default for static
environment content, because it costs nothing at runtime.

Edits to a scene asset SHALL propagate to all its instances, for anything an instance does not
override.

#### Scenario: Static content leaves no trace
- **WHEN** a building scene is embedded into a world
- **THEN** its entities SHALL be partitioned into world cells with baked transforms, and the
  runtime SHALL not know a scene was involved

#### Scenario: A moving interior stays whole
- **WHEN** a ship interior must move as one
- **THEN** it SHALL be packed, retaining its local space and streaming as a unit

#### Scenario: Scene edit propagates
- **WHEN** a scene used in twelve places is edited
- **THEN** all twelve instances SHALL reflect the change except where overridden

### Requirement: Schema versioning and migration
Every serializable type SHALL carry a **schema version**, incremented when its fields change in a
way that requires migration.

When loaded tagged data's version is lower than the current one, registered **migration functions**
SHALL be applied in sequence, operating on the value record.

Migrations SHALL be pure data transformations that do not require the old code to still exist.

Schema version SHALL be independent of type identity: a type's `TypeId` never changes, while its
schema version advances.

#### Scenario: Field rename
- **WHEN** a field is renamed
- **THEN** no migration SHALL be needed, since the field's identity is unchanged; the schema version
  SHALL only advance when the data's *meaning* or *shape* changes

#### Scenario: Missing migration
- **WHEN** data is older than the oldest registered migration
- **THEN** the load SHALL fail with a diagnostic naming the type and versions, rather than
  producing partially initialised data

#### Scenario: Newer data in an older build
- **WHEN** data's schema version exceeds the build's
- **THEN** the load SHALL fail clearly rather than misinterpreting fields

### Requirement: Value-level migration
Migration SHALL operate on a **value record** — a mapping from `FieldId` to encoded value — and
SHALL NOT require constructing an instance of an older version of the type, which no longer exists
in the code.

The load path SHALL be:

```
serialized record → value record → migration chain → current schema → native object
```

The value record SHALL exist only during migration and tooling, and SHALL NOT appear in runtime
hot paths.

Migrations SHALL be classified:

| Class | Examples | Author |
|---|---|---|
| Automatic | Rename, add with default, remove, safe numeric widening | None required; identity handles it |
| Generated | Enum value remap, container kind change, wrapping in an optional | The generator, from a declaration |
| Custom | One field split into several, unit change, semantic change | A developer, written against the value record |

Migrations SHALL apply to **every form of tagged data** addressed by these identifiers: assets,
scenes, prefabs, **prefab overrides**, and saves. A migration that updates an asset but drops the
overrides on it SHALL be a defect.

#### Scenario: Splitting a field
- **WHEN** `health` becomes `currentHealth` and `maxHealth`
- **THEN** a custom migration SHALL read the old value from the value record and write both new
  fields, without any version-1 type existing

#### Scenario: Overrides migrate with the type
- **WHEN** a migrated field is the target of prefab instance overrides
- **THEN** those overrides SHALL be migrated to the new identifiers, and SHALL NOT be discarded

#### Scenario: Renames need no migration
- **WHEN** a field is renamed
- **THEN** no migration SHALL be required, because identity is unchanged

### Requirement: Unknown data is preserved
In **tagged** data, a field whose `FieldId` is not present in the current schema SHALL be
**preserved** through a load and save round trip by default, not discarded.

This SHALL apply to data written by a newer build, by a build with additional modules enabled, or
by a plugin that is not currently loaded — so an editor without a plugin does not silently strip
that plugin's data from every file it touches.

Preservation SHALL be bounded and reportable: preserved unknown data SHALL be visible in
diagnostics, and a project SHALL be able to purge it deliberately.

Cooked data SHALL NOT preserve unknown fields, since it has no tags and no evolution.

#### Scenario: Editing without a plugin
- **WHEN** a scene containing a plugin's components is opened and saved in a build where that
  plugin is disabled
- **THEN** the plugin's data SHALL be written back unchanged rather than stripped

#### Scenario: Tombstoned field is not resurrected
- **WHEN** preserved data refers to a tombstoned identifier
- **THEN** it SHALL remain preserved as unknown data and SHALL NOT be reinterpreted as any current
  field

### Requirement: Scene and prefab cooking
Cooking SHALL resolve the authoring graph — nested prefabs, variants, scene instances, and
overrides — and produce runtime data in which no authoring structure survives:

1. Resolve nesting, variants, and overrides to a concrete entity graph
2. Apply exposed parameter bindings
3. Validate references and reject cycles
4. Flatten hierarchy that is not needed at runtime
5. Assign persistent identities
6. Emit **entity templates** for prefabs, and hand world content to the world cooker for spatial
   partitioning (see `world-partition-and-streaming`)

Editor-only data — names, folder organisation, gizmo settings, selection state, comments — SHALL
NOT be cooked into runtime data unless a runtime consumer requires it.

Prefab provenance SHALL be retained in development builds for live update, and stripped from
shipping builds.

#### Scenario: Fast instantiation
- **WHEN** a cooked prefab with 200 entities is instantiated at runtime
- **THEN** entities SHALL be bulk-allocated into their archetypes and data copied in blocks,
  without per-entity reflection

#### Scenario: Editor keeps the authoring form
- **WHEN** the editor loads a scene
- **THEN** it SHALL use the authoring form so overrides and prefab links remain editable

#### Scenario: Authoring structure does not reach the runtime
- **WHEN** an embedded scene instance containing nested prefab instances is cooked into a world
- **THEN** the runtime SHALL see partitioned entity data, with no scene instance, prefab instance,
  or override structure remaining

### Requirement: Hierarchy flattening
Cooking SHALL determine, per parent-child relationship, whether the relationship is **needed at
runtime** — because something animates, detaches, moves, or queries it — or is purely an authoring
convenience.

Relationships that are not needed SHALL be removed and the child's world transform baked, so that
authored organisational hierarchy costs nothing at runtime.

Relationships that are needed SHALL be retained.

The decision SHALL be overridable per entity, and the cook report SHALL state how many
relationships were flattened.

#### Scenario: Organisational nesting costs nothing
- **WHEN** a building contains windows containing frames, none of which move independently
- **THEN** the relationships SHALL be flattened and transforms baked

#### Scenario: Moving parts keep their hierarchy
- **WHEN** a turret's barrel rotates relative to its base
- **THEN** the relationship SHALL be retained

### Requirement: Entity templates and batch spawning
Cooking SHALL produce, for each prefab, an immutable **entity template**: archetype blocks with
prepared component data, plus the relationships that must exist at runtime.

Runtime spawning SHALL consume the template directly: allocate chunks, bulk copy component data,
fix up intra-template references, apply spawn parameters. Deep prefab inheritance, variant
resolution, and override application SHALL NOT occur at runtime.

Spawning SHALL support **batch spawning**: many instances of one template created in one operation,
with per-instance transforms and parameters supplied as arrays.

Spawn parameters SHALL be resolved to identifiers at cook or load time.

#### Scenario: A hundred-entity prefab spawns as a copy
- **WHEN** a prefab containing a hundred entities is spawned
- **THEN** its entities SHALL be bulk-allocated and copied, with no per-entity reflection and no
  override resolution

#### Scenario: Spawning a thousand of the same thing
- **WHEN** a thousand instances of one template are spawned in one frame
- **THEN** they SHALL be created in a batch rather than by a thousand separate spawn calls

### Requirement: Live prefab update
In development builds, editing a prefab while the game is running SHALL update existing instances
by diffing the previous compiled template against the new one.

Update SHALL be governed by field classification: `Authoring` fields updated, `RuntimeState` and
`PersistentState` preserved, `Derived` recomputed. Added components SHALL be added, removed
components removed, and instance overrides reapplied.

Instances whose state cannot be reconciled SHALL be reported rather than silently reset, and the
policy SHALL be configurable per prefab.

Shipping builds SHALL retain no prefab link, and this behaviour SHALL be absent from them.

#### Scenario: Live edit preserves gameplay state
- **WHEN** a designer changes a robot prefab's maximum health while the game runs
- **THEN** existing robots SHALL take the new maximum and keep their current health

#### Scenario: Shipping builds carry no provenance
- **WHEN** a shipping build spawns a prefab
- **THEN** the resulting entities SHALL carry no prefab link and no override data
