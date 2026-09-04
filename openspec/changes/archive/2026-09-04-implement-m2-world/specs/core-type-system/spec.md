## MODIFIED Requirements

### Requirement: Reflection is control plane, not hot path
Reflection SHALL be used for control-plane work: editor inspection, serialization, migration,
schema generation, dynamic registration, bindings, and debugging.

Work executed per entity per frame SHALL use **typed generated code**, not reflection: field
iteration, offset arithmetic, and dynamic dispatch SHALL NOT appear in per-entity hot paths.

Where a subsystem needs dynamic behaviour at scale — replication, animation property tracks,
serialization of cooked data — reflection SHALL be used at **build or setup time** to generate or
resolve a specialised path that runs without it.

**Lookup complexity is part of the contract.** Type and field lookup SHALL NOT be linear in the
number of registered types or fields. A record decode SHALL be linear in the size of the record, not
in the product of its field count and the type's field count.

"Control plane, not hot path" is a statement about *where* reflection is called from, and it stops
being a defence the moment a scene load calls it once per field per entity. The reflected path that
asset loading and scene instantiation take SHALL be **measured** against a type set representative
of a real project, and the measurement recorded, rather than assumed to be off the hot path because
the specification says so.

#### Scenario: Lookup does not degrade with the registry
- **WHEN** the number of registered types grows by an order of magnitude
- **THEN** type and field lookup cost SHALL be substantially unchanged

#### Scenario: Decode is linear in the record
- **WHEN** a record carrying many fields is decoded
- **THEN** the cost SHALL be linear in the record's size rather than quadratic in its field count

#### Scenario: Queries carry no reflection cost
- **WHEN** a system iterates a million entities through a typed query
- **THEN** no reflection lookup, field enumeration, or dynamic value conversion SHALL occur

#### Scenario: Dynamic behaviour is resolved once
- **WHEN** an animation clip animates a reflected field
- **THEN** the binding SHALL be resolved to a direct accessor once, and sampling SHALL not perform
  a reflection lookup per frame
