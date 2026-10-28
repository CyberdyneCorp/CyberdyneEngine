## ADDED Requirements

### Requirement: A world's step never outgrows its temporary memory
A physics backend that reserves per-world temporary memory SHALL size it from the world's declared
capacities and from what its solver takes for each — not from an estimate — so that a world created
with any valid capacities steps every contact it was declared to hold.

#### Scenario: A small world's first contact
- **WHEN** a world of 128 bodies at the default pair and contact capacities steps a dynamic box onto a static floor
- **THEN** the step SHALL succeed and the box SHALL come to rest on the floor
