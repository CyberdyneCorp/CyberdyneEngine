# `src/core/reflect/` — the type registry and persistent identity

Layer 0. Governed by `core-type-system`. Sections 1.1 and 1.2 of M1.

Two things live here, and the second is why this module is on the critical path of the milestone.

**Reflection** — a `TypeRegistry` mapping a `TypeId` to type metadata: name, size, alignment,
relocatability, construction and destruction thunks, field descriptors, and strongly typed
attributes. It is opt-in by declaration, not by inheritance: there is no `Object` base class, a
reflected component stays a plain standard-layout struct, and nothing uses `typeid` or
`dynamic_cast`, both of which the engine compiles without.

**Identity** — `TypeId` and `FieldId` are opaque numbers assigned once and recorded in the committed
manifest at `identity/manifest.toml`. They are not derived from names, and they are never recycled.
Every scene, prefab override, save, animation binding and replication schema written from M1 onward
encodes them, which is why they land now rather than later.

## The pieces

| Header | What it is |
|---|---|
| `annotations.h` | `CY_REFLECT_TYPE` and `CY_REFLECT_FIELD`. Includes nothing, expands to nothing outside the generator. |
| `ids.h` | `TypeId`, `FieldId`. No constructor from a name, no hash helper — see below. |
| `attributes.h` | The attribute table as typed structures, plus `find_custom<T>()` for a module's own. |
| `type_info.h` | `FieldInfo`, `TypeInfo`, and `TypedAccessor` — the resolved-once hot-path shape. |
| `registry.h` | `TypeRegistry`, `default_registry()`, `type_of<T>()`. |
| `control_plane.h` | `CY_REFLECT_HOT_REGION` and the violation counter. |
| `serialize.h` | Round-tripping records addressed by `FieldId`, and opaque preservation of unknown types. |
| `demo/types.h` | The reflected corpus M1 owns, until M2's components arrive. |

`generated/` holds the generator's output and is **committed**. `reflect_attributes.toml` declares
this module's own `Streaming` attribute, as an example of the mechanism any module may use.

## Why identity is a counter and not a hash

Anything derived from a name changes when the name changes, and a rename is precisely the event the
manifest exists to survive. A hash also collides silently where a counter cannot — and a collision
in this particular number produces data that loads successfully and is wrong, which is the failure
mode with no diagnostic. `ids.h` therefore offers no way to compute an identifier at all.

## Why the generated code is committed

`identity/manifest.toml` is committed, so the C++ that encodes those identifiers is committed beside
it. Three consequences, all of them wanted:

* building the engine does not require libclang, so a contributor who changes nothing annotated pays
  nothing for the generator;
* an identity change is reviewable as a diff of the manifest *and* of the code that encodes it;
* `just generate-check` is a currency check over committed files, which makes staleness a merge gate
  rather than a local surprise.

The generator still runs as a build step when the frontend is present (`CY_REFLECT_GENERATE`,
detected at configure time), so editing an annotated header regenerates within the build that
compiles it.

## Reflection is control plane, not hot path

`core-type-system` requires it and task 1.1.4 requires it be *checked rather than asserted*. Both
halves are implemented:

* **Structural** — a per-frame path holds a `TypedAccessor<T>`, resolved once from a `FieldId` and
  holding a byte offset. It carries no registry pointer and exposes no lookup, so a path written
  against one cannot reach reflection through it.
* **Detected** — `CY_REFLECT_HOT_REGION("label")` marks a per-entity or per-frame region, and every
  reflected lookup inside one increments `control_plane_violations()`. That counter is a plain
  atomic, live in **all four configurations** — an assertion would be compiled out of Profile and
  Shipping, which are the two configurations where nobody would notice the violation.

`tests/test_control_plane.cpp` asserts zero violations across a typed per-entity loop and exactly one
for a deliberate violation, because a check that can only pass is not a check.

## Adding a reflected type

1. Annotate it. The header must include `annotations.h` and as little else as possible — the
   generator re-parses everything a reflected header includes, and the M1 spike measured a ninefold
   increase in cold generation from two standard-library includes per header at an identical type
   count.
2. Name the header in that module's `CY_REFLECT_HEADERS` list in CMake. Explicitly; there is no glob.
3. Call `cy::reflect::register_generated_types()` once, at startup.
4. Run `just generate-headers` and commit `identity/manifest.toml` and `generated/` with the change.

Three separate acts, each visible in a diff. Nothing is reflected by accident.

## Recipes

```
just generate-headers     regenerate the metadata and append any new identifiers
just generate-check       fail if the metadata is stale, or if regeneration is not reproducible
just quality-identity     the identity gate: nothing unassigned, removed silently, or moved
ctest -R reflect          the tests below
```

## Tests

`tests/test_registry.cpp` — opt-in registration, lookup, and the promise that annotating a struct
changes neither its size nor its layout, asserted against an unannotated twin.

`tests/test_attributes.cpp` — every attribute in the specification's table read back as typed data,
including a module's own.

`tests/test_control_plane.cpp` — the two halves of task 1.1.4, in every configuration.

`tests/test_roundtrip.cpp` — the committed goldens under `tests/golden/`: a byte-for-byte encoding
golden, the same values written in a different field order and carrying a field this build has never
declared, and a record whose type is unknown and must survive a re-save.

`tests/test_generator.py` — the generator, the manifest and the gate as programs. Most of what this
module promises about identity is a *build failure*, and a build failure cannot be asserted from
inside the build it fails.

**Governed by**: `core-type-system`. See also `design.md` §1 and §2, and `identity/README.md`.
