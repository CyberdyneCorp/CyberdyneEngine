## MODIFIED Requirements

### Requirement: Entity templates and batch spawning
Cooking SHALL produce, for each prefab, an immutable **entity template**: archetype blocks with
prepared component data, plus the relationships that must exist at runtime.

Runtime spawning SHALL consume the template directly: allocate chunks, bulk copy component data,
fix up intra-template references, apply spawn parameters. Deep prefab inheritance, variant
resolution, and override application SHALL NOT occur at runtime.

Spawning SHALL support **batch spawning**: many instances of one template created in one operation,
with per-instance transforms and parameters supplied as arrays.

Spawn parameters SHALL be resolved to identifiers at cook or load time.

**The cooker SHALL emit the reference sites beside each archetype block** — the column index and
the byte offset of every entity-typed field within that column's component — so that fixup is a
strided pass over known columns rather than a query about what each row contains.

This is a correctness requirement for the flattening guarantee, not an optimisation. Measured on a
102,000-entity cell, asking the component registry per row which columns hold references costs
**4.7–5.2× more** than a cooked site table (268 µs against 58 µs), and that comparison already
assumes a constant-time registry lookup. Without the table, activation degrades into exactly the
per-entity reflection walk that cooking exists to eliminate — and with it, activation is one memcpy
per chunk plus two linear passes.

#### Scenario: Activation does not consult the registry per row
- **WHEN** a cooked cell is activated
- **THEN** reference fixup SHALL visit only the sites the cooker recorded, and SHALL NOT query the
  component registry per entity

#### Scenario: The cost is linear in what was actually cooked
- **WHEN** a cell containing archetypes with no entity references is activated
- **THEN** those archetypes SHALL activate as a copy and a key pass, with no reference work at all

#### Scenario: A hundred-entity prefab spawns as a copy
- **WHEN** a prefab containing a hundred entities is spawned
- **THEN** its entities SHALL be bulk-allocated and copied, with no per-entity reflection and no
  override resolution

#### Scenario: Spawning a thousand of the same thing
- **WHEN** a thousand instances of one template are spawned in one frame
- **THEN** they SHALL be created in a batch rather than by a thousand separate spawn calls
