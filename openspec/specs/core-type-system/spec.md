# core-type-system Specification

## Purpose

Defines CyberdyneEngine's reflection and value model: the compile-time type registry that makes
components, resources, and node types introspectable; the `Var` dynamic value used at the
scripting and serialization boundaries; generational handles; and the event/signal mechanism.

This is the substrate the editor inspector, serializer, C ABI, Swift overlay, and network
replication all build on. It is deliberately **not** a class hierarchy with a common `Object`
base: reflection is opt-in and data-oriented, so plain structs stay plain.

## Requirements

### Requirement: Opt-in reflection registry
The engine SHALL provide a `TypeRegistry` mapping a stable `TypeId` to type metadata: name,
size, alignment, whether the type is trivially relocatable, construction and destruction
thunks, field descriptors, attribute metadata, and optional method descriptors.

Types SHALL opt in by declaration rather than by inheritance, using a reflection macro or a
`cy::reflect<T>()` specialisation, so a reflected component remains a standard-layout struct
with no vtable and no hidden fields.

`TypeId` SHALL be a 64-bit hash of the fully qualified type name, stable across builds and
platforms so serialized data and network messages remain valid.

#### Scenario: Component stays a POD
- **WHEN** a component struct is registered for reflection
- **THEN** `sizeof` and the memory layout SHALL be unchanged, and the type SHALL remain usable in
  packed ECS chunk storage

#### Scenario: Field enumeration
- **WHEN** the inspector displays a reflected type
- **THEN** it SHALL enumerate fields with name, `TypeId`, byte offset, and attributes, and read
  or write them through the offset without type-specific code

#### Scenario: Stable ids across builds
- **WHEN** a scene serialized by one build is loaded by another with the same type names
- **THEN** every `TypeId` SHALL resolve identically

#### Scenario: Unknown type on load
- **WHEN** serialized data references a `TypeId` that is not registered
- **THEN** the data SHALL be preserved verbatim as an opaque blob so re-saving does not destroy
  it, and a diagnostic SHALL name the missing type

### Requirement: Field attributes
Field descriptors SHALL carry attributes that drive tooling without the tooling knowing the type:

- `Range(min, max, step)`, `Enum(names)`, `Flags(names)` — inspector presentation
- `Hidden`, `ReadOnly`, `Category(name)`, `Tooltip(text)`
- `Transient` — excluded from serialization
- `Replicated(condition)` — participates in network replication
- `AssetRef(kind)` — the field holds an asset id of a given kind
- `Unit(kind)` — metres, radians, seconds, for display and conversion

#### Scenario: Inspector renders from attributes alone
- **WHEN** a float field is annotated `Range(0, 1)`
- **THEN** the inspector SHALL render a slider bounded to that range with no editor-side code for
  that specific type

#### Scenario: Transient field is not saved
- **WHEN** a component holds a runtime-only cache annotated `Transient`
- **THEN** it SHALL be skipped by serialization and by replication

### Requirement: Dynamic value type
`Var` SHALL be a tagged dynamic value used only at boundaries — scripting calls, serialized
text, editor edits, and network payloads — and SHALL NOT be used for per-entity runtime storage.

`Var` SHALL cover: `Nil`, `Bool`, `Int` (i64), `Float` (f64), `String`, `Vec2/3/4`,
`IVec2/3/4`, `Quat`, `Mat3`, `Mat4`, `Transform`, `Color`, `Aabb`, `Rect`, `Plane`, `Handle`,
`EntityId`, `AssetId`, `Array`, `Dict`, `Bytes`, and `Callable`.

Values larger than the inline payload SHALL be heap-allocated from a pool with copy-on-write
semantics for the container kinds.

#### Scenario: Boundary use only
- **WHEN** gameplay data is stored per entity
- **THEN** it SHALL be a typed component field, not a `Var`; `Var` appears only when crossing to
  script, disk, or the wire

#### Scenario: Round-trip fidelity
- **WHEN** a typed field is converted to `Var` and back
- **THEN** the value SHALL be bit-identical for all scalar and math types

#### Scenario: Type coercion is explicit
- **WHEN** a `Var` holding a `Float` is assigned to an `Int` field
- **THEN** the conversion SHALL be performed only through an explicit coercion API that reports
  narrowing, never silently

### Requirement: Generational handles
Runtime objects owned by servers SHALL be addressed by `Handle<Tag>`: a 64-bit value packing a
32-bit slot index and a 32-bit generation counter.

Handle pools SHALL allocate slots from chunked storage whose chunk pointers never move, so a
pointer obtained from a handle remains valid for the handle's lifetime. Freeing a handle SHALL
increment the slot's generation.

#### Scenario: Stale handle is detected
- **WHEN** a handle is used after its object has been freed and the slot reused
- **THEN** the generation comparison SHALL fail and the lookup SHALL return null rather than
  aliasing the new object

#### Scenario: Handles are trivially copyable
- **WHEN** a handle is stored in a component
- **THEN** it SHALL be a plain 64-bit value with no refcount, keeping the component trivially
  relocatable

#### Scenario: Cross-thread handle validity
- **WHEN** a handle is resolved concurrently with an unrelated allocation in the same pool
- **THEN** the resolution SHALL be safe, with pool growth published atomically

### Requirement: Asset ids are distinct from handles
`AssetId` SHALL be a 128-bit stable identifier for content (see `core-assets-and-io`), distinct
from runtime `Handle`s. Assets are referenced by id in serialized data; handles are runtime-only
and never serialized.

#### Scenario: Serialized reference survives a reload
- **WHEN** a scene referencing a mesh is saved and reloaded in a new process
- **THEN** the reference SHALL resolve through the `AssetId`, and the runtime handle MAY differ

### Requirement: Events and signals
The engine SHALL provide two decoupled notification mechanisms:

- **Typed event channels** — `EventChannel<T>` with per-frame double-buffered queues, the
  primary mechanism for system-to-system communication in ECS
- **Signals** — named, argument-typed emitters on nodes for the authoring and scripting layer,
  connectable to `Callable`s with `Deferred`, `OneShot`, and `Persist` flags

#### Scenario: Systems communicate without coupling
- **WHEN** the collision system detects a hit
- **THEN** it SHALL write a `CollisionEvent` to a channel, and any number of reader systems SHALL
  observe it in the next stage without either side knowing the other

#### Scenario: Event lifetime is bounded
- **WHEN** an event is written in frame N
- **THEN** it SHALL be readable through the end of frame N+1 and then discarded, so a missed read
  cannot leak memory

#### Scenario: Deferred signal
- **WHEN** a signal is emitted with `Deferred`
- **THEN** invocation SHALL be queued and run at the next flush point, not inline

#### Scenario: Connection to a destroyed target
- **WHEN** a connected node or entity is destroyed
- **THEN** its connections SHALL be removed during destruction so emission never touches freed
  memory

### Requirement: Callable
`Callable` SHALL uniformly represent: a free function, a member function bound to a handle, a
script function in the Swift overlay, and a bound-argument wrapper over any of these.

Invocation SHALL take `Var` arguments and return `Expected<Var, CallError>`, with `CallError`
distinguishing "no such method", "wrong argument count", "wrong argument type", and "target
invalid".

#### Scenario: Script callable survives reload
- **WHEN** the Swift game module is hot-reloaded
- **THEN** callables referring to script functions SHALL be re-resolved by name, or reported
  invalid if the function no longer exists

### Requirement: String interning
The engine SHALL provide `Name`: an interned, immutable string with O(1) comparison and hashing,
used for type names, field names, node names, signal names, and animation track paths.

`Name` SHALL be cheap to copy (a pointer or index) and SHALL support compile-time construction
from string literals.

#### Scenario: Name comparison is a pointer compare
- **WHEN** two `Name`s are compared
- **THEN** the comparison SHALL not touch character data

#### Scenario: Interning is thread-safe
- **WHEN** two threads intern the same string concurrently
- **THEN** both SHALL receive the same `Name` and only one entry SHALL be stored

### Requirement: Diagnostics
`core/diagnostics` SHALL define the engine's error vocabulary: `CY_ASSERT`, `CY_ASSERT_MSG`,
`CY_VERIFY` (evaluated in all builds), `CY_CHECK_RETURN`, `CY_CHECK_RETURN_V`, `CY_UNREACHABLE`,
`CY_LOG_INFO/WARN/ERROR`, and `CY_LOG_ONCE` variants.

Log records SHALL carry a category, severity, source location, and optional structured fields,
and SHALL be routed to registered sinks: console, rotating file, the editor log, and the remote
debugger.

#### Scenario: Assertion in a shipping build
- **WHEN** `CY_ASSERT` is reached in a build with `CY_DEVELOPMENT` off
- **THEN** it SHALL compile to nothing, while `CY_VERIFY` SHALL still evaluate its expression

#### Scenario: Error reaches every sink
- **WHEN** an error is logged while the editor is attached
- **THEN** it SHALL appear on the console, in the log file, and in the editor's log panel with
  file, line, and category
