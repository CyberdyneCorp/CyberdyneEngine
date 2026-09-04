# `src/core/serialize/` — layer 0

Layer 0, target `cy::core-serialize`, headers `<cy/core/serialize/*.h>`, namespace `cy::serialize`.
Tasks 3.2.2, 3.2.3, 3.2.4 and 3.2.9 of `openspec/changes/implement-m2-world/tasks.md`, governed by
`serialization-and-prefabs` for the two modes and by `core-type-system` for the identity every
record addresses.

One reflection-driven traversal, and three writers over it. A type is serializable because it is
reflected, not because it implements a serializer.

## The map

| Header | What it owns |
|---|---|
| `wire.h` | `WireType`, `ByteWriter`, `ByteReader` — byte order, bounds, and stepping over the unknown |
| `classification.h` | `Purpose`, and the one table turning a field's class into a decision |
| `traversal.h` | `FieldVisitor`, `visit_object()` — the single walk (design.md §6) |
| `value_record.h` | `ValueRecord` — `FieldId` → encoded value; migration's substrate and unknown data's home |
| `tagged.h` | The tagged stream: chunked, versioned, skip-unknown, bounds-checked |
| `text.h` | The canonical text form's lexical layer, and floats that round-trip exactly |
| `migration.h` | `SchemaRegistry` — schema versions, the chain, and override-target migration |
| `cooked.h` | The cooked container: packed columns, build schema identity, reference sites |

## Five decisions worth knowing before changing anything here

**1. The traversal exists once.** `visit_object()` decides which fields are visited — `Transient`,
then the classification table — and in what order. The tagged writer, the text writer and the record
builder are all `FieldVisitor`s passed to it. Three walks would mean `Transient` honoured in two
places and forgotten in the third, and a classification table with three readers that drift.

**2. Unknown data is preserved by construction, not by a code path.** `read_record()` reads into a
`ValueRecord`, which has no schema to check anything against — so a field this build has never heard
of is a field like any other, and writing the record back writes it. There is no branch that could
be forgotten and no flag that could default the wrong way. The same is true of a whole record whose
`TypeId` is unregistered: a disabled plugin's component survives a load and a save byte for byte,
and `test_tagged.cpp` asserts exactly that.

**3. A value record is ordered by `FieldId`, not by insertion.** Two records holding the same values
are byte-identical whatever order they were built in. That is what makes the text form's
"one changed property, one changed line" true *after* a load-and-save round trip rather than only on
the first write.

**4. The canonical text form contains no field names.** A name in the file would make renaming a
field rewrite every scene that touches it, which is the cost the identity model exists to remove.
`TextOptions::annotate` adds names as trailing comments for a human reading a file, and it is off in
the form that is committed. A reference is spelled `@<local>` rather than `#<local>` because `#`
starts a comment — found by the first document that contained one.

**5. Floats are the shortest decimal that round-trips.** `format_f32` tries increasing precision
until parsing the result reproduces the original bits, so `0.1F` is written `0.1` and not
`0.100000001`, and the same float always produces the same characters. `%.9g` unconditionally would
be exact and would make every designer-typed value look like a rounding error.

## Two things that are thinner than they look

* **`FieldKind` covers scalars and enumerations.** A `Vec3` member is `FieldKind::Unsupported` to
  M1's reflection, which maps to `WireType::Bytes` — copied exactly, not interpreted. That is why
  `src/scene/serialization/`'s transform binding is one 40-byte field rather than nine scalars, and
  it is the seam that closes when the generator learns about vectors.
* **There is no reference kind in reflection.** `WireType::LocalRef` and `ExternalRef` are wire
  types this module defines, and what a local id *means* belongs to whoever wrote it. Layer 0 never
  resolves one: `record_to_object` skips a reference field rather than writing a plausible-looking
  wrong answer into it.

## Registration

Declared from `src/CMakeLists.txt` rather than `src/core/CMakeLists.txt`, because that file belonged
to another agent while the scaffold landed. The line belongs beside the other core modules and
moving it there changes nothing else; there is a comment at both ends saying so.
