## MODIFIED Requirements

### Requirement: Hierarchical state hashing
The engine SHALL compute a **state hash** over authoritative state, and the hash SHALL be
**hierarchical**: world, subsystem, archetype, chunk, entity, component, field.

A divergence SHALL be narrowable by descending the hierarchy, so the result of a mismatch is a named
field on a named entity rather than a statement that two numbers differ.

Hashing SHALL cover declared authoritative fields only, and SHALL NOT hash raw memory, padding, or
derived data.

Hash frequency SHALL be configurable: every tick in validation builds, periodically in shipping
lockstep, and on demand.

Where practical, subtree hashes SHALL be maintained incrementally so that periodic hashing does not
cost a full traversal.

**Entity identity is part of the hash.** The hash SHALL fold an entity's identity into its node, so
that two worlds holding identical component values under different entity identifiers hash
differently.

This is deliberate and it has consequences that SHALL be written down rather than discovered. An
identifier is state the moment anything holds a reference to it, and a divergence report has to name
an entity, which it cannot do if identity is outside the hash. In exchange:

- a streaming cell activated after different world history receives different identifiers and
  therefore a different hash, even when its content is identical;
- a replay or snapshot SHALL restore identifiers **verbatim** rather than merely restoring values,
  because restoring equivalent content under fresh identifiers is a divergence by this definition.

What SHALL NOT affect the hash is genuine allocator history that no reference can observe: chunk
membership, packing order within a chunk, and component registration order.

#### Scenario: Identical content under different identifiers diverges
- **WHEN** two worlds hold the same component values on entities with different identifiers
- **THEN** their hashes SHALL differ, and the divergence report SHALL name the entity

#### Scenario: Packing is not state
- **WHEN** the same entities are packed differently across chunks with their identifiers unchanged
- **THEN** the hash SHALL be identical

#### Scenario: A snapshot restores identity, not merely value
- **WHEN** a snapshot is restored
- **THEN** entity identifiers SHALL be restored exactly, and the hash SHALL match the hash taken
  before the snapshot

#### Scenario: Narrowing to a field
- **WHEN** two runs disagree at a tick
- **THEN** descending the hash hierarchy SHALL identify the entity, component, and field that differ

#### Scenario: Raw memory is not the hash
- **WHEN** a component contains padding or a derived cache
- **THEN** they SHALL be excluded from the hash
