## ADDED Requirements

### Requirement: Stable field identity
Every serializable field of a reflected type SHALL have a **`FieldId`**: an identifier stable
across renames, reorderings, and layout changes, recorded in the identity manifest.

Serialized data, prefab overrides, animation property tracks, replication schemas, and migrations
SHALL address fields by `FieldId`. A field's **byte offset** SHALL remain available for native
access and SHALL NEVER appear in serialized data, because layout is a compiler artefact.

Renaming a field SHALL change its name in the manifest and nothing else.

`FieldId` SHALL be unique within its type, and SHALL NOT be reused after a field is removed.

#### Scenario: Renaming a field breaks nothing
- **WHEN** `maxHealth` is renamed to `maximumHealth`
- **THEN** its `FieldId` SHALL be unchanged, and existing scenes, saves, prefab overrides, animation
  tracks, and replication schemas SHALL continue to resolve

#### Scenario: Reordering fields breaks nothing
- **WHEN** fields are reordered or a field is inserted, changing every subsequent offset
- **THEN** serialized data SHALL still resolve, because it addresses identifiers rather than
  offsets

#### Scenario: Offsets are never serialized
- **WHEN** a field's byte offset is required
- **THEN** it SHALL be obtained from the runtime type registry, and SHALL NOT be read from or
  written to serialized data

### Requirement: Identity manifest and tombstones
Type, field, component, enum-value, and other persistent identifiers SHALL be recorded in a
committed **identity manifest**: a source-controlled artefact mapping each identifier to its
current name, its declaring module, and its status.

The manifest SHALL be authoritative. Registration SHALL consult it, and a newly encountered
declaration SHALL be assigned an identifier and appended.

Removing a declaration SHALL leave a **tombstone** recording the identifier, its former name, and
the version in which it was removed. A tombstoned identifier SHALL NEVER be reused, because a
recycled identifier produces data that loads successfully and is wrong.

A **continuous integration gate** SHALL diff the manifest and fail when an existing entry's
identifier changes, so an accidental identity change is a red build rather than a corrupted save
months later.

Deliberate identity changes SHALL be possible through an explicit, reviewed manifest edit.

#### Scenario: Accidental identity change fails the build
- **WHEN** a change would alter the identifier of an existing type or field
- **THEN** the manifest gate SHALL fail, naming the entry and its previous identifier

#### Scenario: Removed identifiers are not recycled
- **WHEN** a field is deleted and a different field is added later
- **THEN** the new field SHALL receive a fresh identifier, and the deleted field's identifier SHALL
  remain tombstoned

#### Scenario: Identity changes are reviewable
- **WHEN** an identity change is genuinely intended
- **THEN** it SHALL appear as a diff of the committed manifest, reviewable like any other change

### Requirement: Reflection generator
Reflection metadata SHALL be produced by a **generator** run as a build step, not written by hand
and not derived from C++ runtime type information.

The generator SHALL consume annotated C++ declarations, parsed with a real C++ frontend rather than
a bespoke text scanner, and SHALL emit: type and field metadata, identity manifest entries,
serialization code, component registration, editor metadata, replication schema inputs, C ABI
descriptors, and Swift binding inputs — all from one declaration, so parallel metadata definitions
cannot drift.

Generation SHALL be incremental and deterministic: identical inputs produce byte-identical outputs,
and a change to one header regenerates only what depends on it.

Annotation SHALL be minimally intrusive: a reflected struct SHALL remain standard-layout with no
base class, no virtual functions, and no hidden fields.

#### Scenario: One declaration, many outputs
- **WHEN** a component is annotated and built
- **THEN** its metadata, serializer, registration, editor presentation, network schema inputs, and
  bindings SHALL all be generated from that single declaration

#### Scenario: No runtime type information
- **WHEN** the engine identifies a type
- **THEN** it SHALL use generated metadata and `TypeId`, and SHALL NOT use `typeid`,
  `dynamic_cast`, or compiler-specific name mangling

#### Scenario: Generation is deterministic
- **WHEN** the generator runs twice on unchanged inputs
- **THEN** it SHALL produce byte-identical outputs, so generated files do not churn in builds or
  caches

### Requirement: Reflection is control plane, not hot path
Reflection SHALL be used for control-plane work: editor inspection, serialization, migration,
schema generation, dynamic registration, bindings, and debugging.

Work executed per entity per frame SHALL use **typed generated code**, not reflection: field
iteration, offset arithmetic, and dynamic dispatch SHALL NOT appear in per-entity hot paths.

Where a subsystem needs dynamic behaviour at scale — replication, animation property tracks,
serialization of cooked data — reflection SHALL be used at **build or setup time** to generate or
resolve a specialised path that runs without it.

#### Scenario: Queries carry no reflection cost
- **WHEN** a system iterates a million entities through a typed query
- **THEN** no reflection lookup, field enumeration, or dynamic value conversion SHALL occur

#### Scenario: Dynamic behaviour is resolved once
- **WHEN** an animation clip animates a reflected field
- **THEN** the binding SHALL be resolved to a direct accessor once, and sampling SHALL not perform
  a reflection lookup per frame

## MODIFIED Requirements

### Requirement: Opt-in reflection registry
The engine SHALL provide a `TypeRegistry` mapping a stable `TypeId` to type metadata: name,
size, alignment, whether the type is trivially relocatable, construction and destruction
thunks, field descriptors, attribute metadata, and optional method descriptors.

Types SHALL opt in by declaration rather than by inheritance, using a reflection annotation or a
`cy::reflect<T>()` specialisation, so a reflected component remains a standard-layout struct
with no vtable and no hidden fields.

`TypeId` SHALL be **assigned once and recorded in the identity manifest**, and SHALL NOT be derived
from the type's name at each build. A name hash MAY be used as the initial value when a type is
first registered; thereafter the manifest is authoritative.

Renaming a type, moving it between namespaces, or relocating it between modules SHALL NOT change
its `TypeId`. Names are metadata; identity is not.

`TypeId` SHALL be stable across builds, platforms, and compilers, so serialized data and network
messages remain valid.

#### Scenario: Component stays a POD
- **WHEN** a component struct is registered for reflection
- **THEN** `sizeof` and the memory layout SHALL be unchanged, and the type SHALL remain usable in
  packed ECS chunk storage

#### Scenario: Field enumeration
- **WHEN** the inspector displays a reflected type
- **THEN** it SHALL enumerate fields with name, `FieldId`, `TypeId`, byte offset, and attributes,
  and read or write them through the offset without type-specific code

#### Scenario: Stable ids across builds
- **WHEN** a scene serialized by one build is loaded by another
- **THEN** every `TypeId` SHALL resolve identically, because both read the same manifest

#### Scenario: Unknown type on load
- **WHEN** serialized data references a `TypeId` that is not registered
- **THEN** the data SHALL be preserved verbatim as an opaque blob so re-saving does not destroy
  it, and a diagnostic SHALL name the missing type, consulting the manifest to report its last
  known name

#### Scenario: Moving a type into a namespace
- **WHEN** a type is moved from the global namespace into a module namespace
- **THEN** its `TypeId` SHALL be unchanged and every asset, save, and network schema referencing it
  SHALL continue to resolve

### Requirement: Field attributes
Field descriptors SHALL carry attributes that drive tooling without the tooling knowing the type.

Attributes SHALL be **strongly typed structures**, not a string-keyed dictionary of string values,
so that the generator validates them at build time and consumers read typed data:

| Attribute | Carries |
|---|---|
| `Range` | Minimum, maximum, step |
| `Enum`, `Flags` | Value names and their persistent values |
| `Hidden`, `ReadOnly` | Presentation flags |
| `Category`, `Tooltip` | Presentation text |
| `Transient` | Excluded from serialization and replication |
| `Replicated` | Encoder, parameters, and send condition |
| `AssetRef` | The asset kind the field references |
| `Unit` | Metres, radians, seconds — for display and conversion |
| `Persistence` | The field classification defined in `serialization-and-prefabs` |

An unknown or malformed attribute SHALL be a build error naming the field, rather than a value
silently ignored at runtime.

Projects and modules SHALL be able to declare their own attribute types, which the generator emits
as typed data like the built-in ones.

#### Scenario: Inspector renders from attributes alone
- **WHEN** a float field is annotated `Range(0, 1)`
- **THEN** the inspector SHALL render a slider bounded to that range with no editor-side code for
  that specific type

#### Scenario: Transient field is not saved
- **WHEN** a component holds a runtime-only cache annotated `Transient`
- **THEN** it SHALL be skipped by serialization and by replication

#### Scenario: Malformed attribute fails the build
- **WHEN** a `Range` attribute is given a minimum greater than its maximum
- **THEN** the build SHALL fail naming the field, rather than producing an inspector control that
  cannot be used

#### Scenario: Custom attribute is typed
- **WHEN** a project declares its own attribute
- **THEN** the generator SHALL emit it as a typed structure and consumers SHALL read it without
  string parsing
