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
| `probe_table.h` | `detail::ProbeTable`, the one open-addressed table under both lookups. |
| `field_index.h` | `FieldIndex` — a type's fields resolved once, looked up in constant time. |
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

The generator's own prerequisites — the pinned PyPI `clang` bindings and a host libclang 18 — are
declared in `deps/host-tools.toml`, appear in `THIRD_PARTY.md`, and are reported by
`just env-doctor`. Until M2 they were declared nowhere, and a clone without them regenerated nothing
and said nothing.

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

## Lookup complexity is part of the contract

M1 shipped a linear scan in three places and argued it was fine: this is control-plane code, the
registry holds hundreds of entries, and anything needing it faster would be doing per-frame work
through reflection. Two of those three were wrong in the same way, and M2's spec delta says so:

> Type and field lookup SHALL NOT be linear in the number of registered types or fields. A record
> decode SHALL be linear in the size of the record, not in the product of its field count and the
> type's field count.

A scene load calls `TypeRegistry::find()` once per record and `TypeInfo::find_field()` once per field
per record. The registry does not have to be on a per-frame path for its size to multiply into
somebody else's loop — with two reflected types nothing noticed, and M2 reflects hundreds.

What changed:

* `TypeRegistry` indexes both `TypeId` and name through an open-addressed table (`probe_table.h`).
  The entries array is still the storage and still iterates in registration order, because a
  registry whose iteration order depended on a hash would make every artefact derived from a walk of
  it depend on one too.
* `FieldIndex` (`field_index.h`) is a type's fields indexed by `FieldId` and by name, built in one
  pass. `TypeRegistry::add()` builds one per registered type, so a loader that has a `TypeId` and a
  stream of records — `TypeRegistry::fields()` — never builds one and never scans.
* `read_record()` takes a `FieldIndex`. Its cost is one probe per field *in the record*. The
  `TypeInfo` overload remains for a caller decoding one record, and builds an index for the call.
* `TypeInfo::find_field()` remains, linear, and is now documented as the one-shot spelling: building
  a hash table to read a single descriptor costs more than the scan it saves.

### The measurement

`tests/test_scaling.cpp` carries M1's implementations as **reference implementations** — the same
arrangement `src/core/math/` uses for its scalar SIMD reference — and measures both on one corpus in
one process, so these numbers are reproducible by running the suite rather than being a claim about
one afternoon. The corpus is synthetic `TypeInfo` data, which is exactly what the generator emits.

Development profile, GCC 13.3 at `-O2`, one x86-64 host, run repeatedly. Where a row shows
a range, that is the spread across runs on an ordinarily busy machine — the scan's cost is far more
variable than the index's, because it is cache-bound and the index is not:

| Measurement | M1: scan | M2: indexed | |
|---|---|---|---|
| `find(TypeId)`, 32 types | 14.2 ns | 3.1 ns | 4.6x |
| `find(TypeId)`, 320 types | 80 ns | 3.1–3.6 ns | 22–26x |
| `find(TypeId)`, 3200 types | 510–1250 ns | 1.6–3.9 ns | 250–760x |
| decode one 256-field record | 9.6–10.8 µs | 0.81–0.93 µs | 11–12x |
| **scene load — 512 types, 20 000 records** | **256–282 ns/record (5.1–5.6 ms)** | **116–125 ns/record (2.3–2.5 ms)** | **2.2x** |

Read the last row honestly: **2.2x, not 300x**, because a 26-field record's decode is real work — a
probe and a `memcpy` per field — and the quadratic term is only the part around it. The dramatic
factors are in the components, and they are what the numbers would look like at a wider type or a
larger registry. The lookup cost is flat across a hundredfold growth in the registry — 0.5x to
1.3x, which is measurement noise around one — where the scan grows 40–90x. That flatness is the
property the specification actually asks for.

Two assertions guard it, and only one of them owns a stopwatch. `longest_probe()` reports the longest
chain either table holds; it is 2, 4 and 13 at 32, 320 and 3200 types — the O(log n) worst case of
linear probing at a load factor of one half, against a scan's O(n) — and that assertion is
deterministic on any machine in any profile. The timed assertions compare ratios within one run, with
bounds far below the measured margins, so the suite fails when the complexity comes back rather than
when the build machine is busy.

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

`tests/test_scaling.cpp` — the complexity contract above, measured against M1's implementations kept
in the test as references. An integration suite: it builds a corpus of thousands of types and decodes
tens of thousands of records, which is the size the measurement has to be taken at.

`tests/test_generator.py` — the generator, the manifest and the gate as programs. Most of what this
module promises about identity is a *build failure*, and a build failure cannot be asserted from
inside the build it fails.

**Governed by**: `core-type-system`. See also `design.md` §1 and §2, and `identity/README.md`.
