## MODIFIED Requirements

### Requirement: Replication schemas
Replicated components SHALL be described by **replication schemas**: declarations of which fields
cross the wire and how each is encoded.

Schemas SHALL identify fields by **`FieldId`** (see `core-type-system`), so that renaming a field
does not invalidate a schema, and so that schema drift means a field genuinely removed or changed
rather than merely renamed.

A schema SHALL specify per field: the encoder (raw, quantised scalar, quantised vector, compressed
quaternion, dictionary index, bitfield), its parameters (range and bit count, or precision), the
send condition, the target filter, and a priority contribution.

Schemas SHALL be **compiled** to serialisation and deserialisation code, not interpreted per field
per entity, so that replicating a thousand instances of a component is a loop over a packed array
with a known encoder.

Schemas SHALL be validated against the reflected component type at cook time; a field that no
longer exists, or a range that cannot represent the field's declared bounds, SHALL be a cook error.

Schema identity SHALL be versioned and verified at connection time, so peers with mismatched
schemas are rejected rather than misinterpreting each other's data.

#### Scenario: Packed serialisation
- **WHEN** 1,000 entities' transforms are replicated
- **THEN** they SHALL be serialised by iterating a packed component array through compiled encoder
  code, not by per-field reflection per entity

#### Scenario: Schema drift is caught
- **WHEN** a replicated component field is removed or its type changed without updating its schema
- **THEN** cooking SHALL fail naming the field; a rename alone SHALL NOT be drift, since the
  schema keys on identity

#### Scenario: Mismatched peers are rejected
- **WHEN** a client with an older schema set connects
- **THEN** the mismatch SHALL be detected at connection and reported, rather than producing
  corrupt state
