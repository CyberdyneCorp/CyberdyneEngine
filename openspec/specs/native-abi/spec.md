# native-abi Specification

## Purpose

Defines the stable, flat **C ABI** that CyberdyneEngine exports. It is the single boundary
between the C++20 engine core and everything outside it: the Swift overlay, native game modules,
editor plugins, and any future language binding.

The design goal is that a module built against version *X* of the ABI keeps working against
version *X + n* without recompilation. That constraint is why the boundary is C and not C++: C++
has no stable ABI across compilers, standard library versions, or even optimisation settings.

(Influence: Godot's GDExtension interface; the versioned function-table pattern is deliberate.)

## Requirements

### Requirement: Flat C interface
The ABI SHALL consist only of C constructs: `extern "C"` functions, opaque pointer handles,
POD structs with explicit layout, fixed-width integer types, and function pointers.

The ABI SHALL NOT expose C++ classes, templates, references, exceptions, `std::` types, virtual
dispatch, or any type whose layout depends on the compiler or standard library.

All ABI symbols SHALL be prefixed `cy_`, all ABI types `Cy`, and all enum constants `CY_`.

#### Scenario: Compiler independence
- **WHEN** the engine is built with Clang and a game module with GCC or MSVC
- **THEN** the module SHALL load and call the engine correctly

#### Scenario: Struct layout is explicit
- **WHEN** a struct crosses the ABI
- **THEN** its layout SHALL be fixed-width, explicitly padded, and asserted with
  `static_assert(sizeof(...))` on both sides

### Requirement: Versioned interface table
The engine SHALL export exactly one symbol for discovery, `cy_get_interface`, returning a
pointer to a versioned interface table. All other functionality SHALL be reached through that
table rather than by direct symbol linkage.

```c
typedef struct CyInterfaceHeader {
    uint32_t abi_major;      /* incompatible changes */
    uint32_t abi_minor;      /* additive changes     */
    uint32_t abi_patch;
    uint32_t table_size;     /* bytes; enables additive growth */
} CyInterfaceHeader;

const CyInterface* cy_get_interface(uint32_t requested_major,
                                    uint32_t requested_minor);
```

The table SHALL be **append-only** within a major version: new function pointers are added at the
end and `table_size` grows. Existing entries SHALL never be reordered, removed, or change
signature.

#### Scenario: Newer engine, older module
- **WHEN** a module built against ABI 1.2 loads into an engine exporting 1.7
- **THEN** `cy_get_interface(1, 2)` SHALL return a table whose first entries match 1.2 exactly,
  and the module SHALL work unmodified

#### Scenario: Older engine, newer module
- **WHEN** a module requires 1.7 and the engine exports 1.4
- **THEN** `cy_get_interface` SHALL return null and the loader SHALL report the version mismatch
  with both version numbers

#### Scenario: Major version change
- **WHEN** an incompatible change is unavoidable
- **THEN** `abi_major` SHALL be incremented, and the engine MAY export multiple major tables
  during a documented transition period

#### Scenario: Signature change is forbidden
- **WHEN** a function's behaviour must change incompatibly within a major version
- **THEN** a new entry SHALL be appended and the old one kept working, rather than the existing
  entry being modified

### Requirement: Module entry point
A native or Swift game module SHALL be a shared library exporting a single entry symbol whose
name is declared in its manifest:

```c
typedef struct CyModuleInit {
    uint32_t abi_major, abi_minor;
    void (*initialize)(CyInitLevel level, void* user_data);
    void (*shutdown)(CyInitLevel level, void* user_data);
    void* user_data;
} CyModuleInit;

bool cy_module_entry(const CyInterface* iface, CyModuleInit* out_init);
```

The manifest (`module.toml`) SHALL declare: module name, entry symbol, minimum ABI version,
per-platform library paths, and whether the module is hot-reloadable.

#### Scenario: Module registers types
- **WHEN** `initialize` is called at the `Scene` level
- **THEN** the module SHALL register its component types, node templates, and behaviours through
  the interface table

#### Scenario: Entry point returns false
- **WHEN** the module cannot initialise (missing dependency, unsupported platform)
- **THEN** `cy_module_entry` SHALL return false and the loader SHALL report it without aborting
  engine startup

### Requirement: Handles and opaque pointers
Runtime objects SHALL cross the ABI as `CyHandle` — a 64-bit generational value — or as opaque
pointers whose lifetime rules are documented per API.

The ABI SHALL NOT expose raw pointers into ECS chunk storage across a frame boundary, because
that storage moves on archetype changes.

#### Scenario: Handle survives storage movement
- **WHEN** an entity changes archetype and its component data moves
- **THEN** handles held by a module SHALL remain valid and resolve to the new location

#### Scenario: Borrowed pointer is scoped
- **WHEN** the ABI hands out a pointer into component storage for bulk access
- **THEN** its validity SHALL be documented as ending at the next structural-change flush, and
  development builds SHALL detect use past that point

### Requirement: Value marshalling
Dynamic values SHALL cross the ABI as `CyVar`: a tagged union with a type tag and a fixed-size
payload, with values exceeding the payload heap-allocated and reference counted by the engine.

The ABI SHALL provide typed fast paths (`cy_component_get_f32`, `cy_component_set_vec3`, …) that
bypass `CyVar` entirely, so hot paths do not marshal.

Ownership rules SHALL be explicit per function: whether the caller must destroy a returned
`CyVar`, and whether arguments are borrowed or consumed.

#### Scenario: Hot path avoids CyVar
- **WHEN** Swift code updates a transform each tick
- **THEN** the generated overlay SHALL call the typed `cy_transform_set` entry, not the generic
  property-set path

#### Scenario: Ownership is documented and checked
- **WHEN** a function returns an owned `CyVar`
- **THEN** the header SHALL document it and development builds SHALL detect leaks of returned
  values

### Requirement: Errors do not cross as exceptions
No exception, C++ or otherwise, SHALL propagate across the ABI. Fallible functions SHALL return
a `CyResult` status code, with a thread-local last-error message retrievable via
`cy_get_last_error`.

#### Scenario: Failure is reported by return value
- **WHEN** a module requests a component the entity does not have
- **THEN** the call SHALL return `CY_RESULT_NOT_FOUND` and leave the output untouched

#### Scenario: Module throws
- **WHEN** a module's callback raises a language-level error (a Swift trap)
- **THEN** it SHALL be caught at the module's own boundary before returning to the engine; the
  engine SHALL not attempt to unwind through the ABI

### Requirement: Callbacks into modules
The engine SHALL invoke module code only through function pointers registered by the module,
each carrying an opaque `void* user_data`.

Every callback SHALL document its thread role and what it may touch.

#### Scenario: Callback thread role
- **WHEN** a module registers a system callback
- **THEN** the ABI documentation SHALL state that it may run on a job worker and must respect the
  system's declared component access

#### Scenario: Module unloaded with callbacks registered
- **WHEN** a module is unloaded
- **THEN** every callback it registered SHALL be unregistered first, so no dangling function
  pointer remains

### Requirement: Type registration from modules
Modules SHALL be able to register: component types (name, size, alignment, field descriptors,
construct/destruct/copy thunks), node templates, systems with access declarations, resource
types, and editor inspector metadata.

Registered types SHALL be indistinguishable from engine-native types to the serializer,
inspector, and replication system.

#### Scenario: Module component is serializable
- **WHEN** a module registers a component with reflected fields
- **THEN** scenes containing it SHALL serialize, inspect, and replicate without engine changes

#### Scenario: Module unloaded with live instances
- **WHEN** a module is unloaded while entities hold its components
- **THEN** those components SHALL be converted to opaque preserved blobs so the data survives
  until the module returns

### Requirement: Hot reload
Modules declared hot-reloadable SHALL support unload and reload without restarting the engine.

Reload SHALL: quiesce at a frame boundary, serialize live instances of module-owned types, call
`shutdown`, unload the library, load the new library, call `initialize`, and restore instances
through schema migration.

#### Scenario: Behaviour reload preserves state
- **WHEN** a Swift behaviour is edited and its module reloaded
- **THEN** entity state SHALL be preserved and the new code SHALL run against it from the next
  frame

#### Scenario: Incompatible reload
- **WHEN** the new module removes a component type that live entities still use
- **THEN** the reload SHALL be rejected with a diagnostic and the old module retained

### Requirement: ABI compatibility testing
The build SHALL generate a machine-readable description of the ABI (function names, signatures,
struct layouts, enum values) and CI SHALL diff it against the committed baseline.

Any change other than an append SHALL fail CI unless accompanied by an explicit, reviewed
approval entry recording the rationale and the version bump.

#### Scenario: Accidental break is caught
- **WHEN** a developer changes an existing ABI function's parameter type
- **THEN** CI SHALL fail with a diff showing the incompatible change

#### Scenario: Additive change passes
- **WHEN** a new function is appended and `abi_minor` incremented
- **THEN** CI SHALL accept it and update the baseline
