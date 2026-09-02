# serialization-and-prefabs Specification

## Purpose

Defines how scenes and prefabs are stored, instantiated, overridden, and versioned. The model
takes **prefabs with per-instance property overrides and nesting** from Unity, **scene
inheritance** from Godot, and **explicit, reviewable text serialization** from both.

## Requirements

### Requirement: Reflection-driven serialization
Serialization SHALL be driven entirely by the type registry: a type is serializable because it is
reflected, not because it implements a serializer.

Fields marked `Transient` SHALL be skipped. Fields absent from serialized data SHALL take the
type's default value, so adding a field does not invalidate existing data.

#### Scenario: New field is backward compatible
- **WHEN** a component gains a field and an older scene is loaded
- **THEN** the new field SHALL take its default and the load SHALL succeed

#### Scenario: Removed field is ignored
- **WHEN** serialized data contains a field the type no longer has
- **THEN** it SHALL be ignored with a development-build warning, not an error

#### Scenario: Unknown component is preserved
- **WHEN** a scene references a component type that is not registered (disabled module)
- **THEN** its data SHALL be retained as an opaque blob and written back unchanged on save

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
- **THEN** the stale override SHALL be dropped on load with a development-build warning

### Requirement: Prefab variants
A **variant** SHALL be a prefab whose base is another prefab, storing only its own overrides.
Variants SHALL be nestable to arbitrary depth.

#### Scenario: Variant inherits later base changes
- **WHEN** the base prefab changes a property the variant does not override
- **THEN** the variant and its instances SHALL pick up the change

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

### Requirement: Schema versioning and migration
Every serializable type SHALL carry a schema version. When loaded data's version is lower than
the current one, registered **migration functions** SHALL be applied in sequence.

Migrations SHALL be pure data transformations that do not require the old code to still exist.

#### Scenario: Field rename
- **WHEN** a field is renamed and a migration from version 1 to 2 is registered
- **THEN** older data SHALL be migrated on load and re-saved in the new form

#### Scenario: Missing migration
- **WHEN** data is older than the oldest registered migration
- **THEN** the load SHALL fail with a diagnostic naming the type and versions, rather than
  producing partially initialised data

#### Scenario: Newer data in an older build
- **WHEN** data's schema version exceeds the build's
- **THEN** the load SHALL fail clearly rather than misinterpreting fields

### Requirement: Scene and prefab cooking
Scenes and prefabs SHALL be cooked into a runtime form in which prefab instantiation is a bulk
operation: the cooked scene contains flattened archetype layouts and prepared component blobs so
instantiation is largely a memory copy plus reference fixup.

#### Scenario: Fast instantiation
- **WHEN** a cooked prefab with 200 entities is instantiated at runtime
- **THEN** entities SHALL be bulk-allocated into their archetypes and data copied in blocks,
  without per-entity reflection

#### Scenario: Editor keeps the authoring form
- **WHEN** the editor loads a scene
- **THEN** it SHALL use the authoring form so overrides and prefab links remain editable
