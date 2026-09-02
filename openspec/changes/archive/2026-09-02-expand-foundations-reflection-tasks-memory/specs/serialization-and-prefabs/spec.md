## ADDED Requirements

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

## MODIFIED Requirements

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
